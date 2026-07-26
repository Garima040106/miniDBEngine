#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tinylsm/database.h"
#include "tinylsm/kv_store.h"
#include "tinylsm/sstable.h"
#include "test_util.h"

using tinylsm::Database;
using tinylsm::Entry;
using tinylsm::KVStore;
using tinylsm::SSTableReader;
using tinylsm::WriteSSTable;
using tinylsm::test::TempDir;

namespace {
// Small enough that a couple of short puts push a MemTable over it,
// so tests can force a flush without writing megabytes of data.
constexpr size_t kTinyFlushThreshold = 32;

// Even smaller: forces a flush on essentially every put, so tests that
// need many SSTables don't need many keys per SSTable.
constexpr size_t kFlushEveryWrite = 1;
constexpr size_t kCompactAtThree = 3;

// Asserts exactly one SSTable file exists in `dir` and returns a reader
// for it - used by tests that want to inspect compacted output directly,
// below the Database's get()/scan() (which would just report "not
// found" for a dropped tombstone, not prove it was dropped).
SSTableReader FindSingleSSTable(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> sstables;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".sst") {
            sstables.push_back(entry.path());
        }
    }
    if (sstables.size() != 1) {
        throw std::runtime_error("expected exactly one SSTable, found " +
                                  std::to_string(sstables.size()));
    }
    return SSTableReader(sstables.front());
}
}  // namespace

TEST_CASE("Database GET on a never-written key returns nullopt", "[database]") {
    TempDir dir;
    Database db(dir.path);
    REQUIRE(db.get("nope") == std::nullopt);
}

TEST_CASE("Database put/get/remove round-trip through the KVStore interface", "[database]") {
    TempDir dir;
    std::unique_ptr<KVStore> store = std::make_unique<Database>(dir.path);

    store->put("key", "value");
    REQUIRE(store->get("key") == "value");

    store->remove("key");
    REQUIRE(store->get("key") == std::nullopt);
}

TEST_CASE("Database scan reflects the current state and excludes tombstones", "[database]") {
    TempDir dir;
    Database db(dir.path);
    db.put("a", "1");
    db.put("b", "2");
    db.put("c", "3");
    db.remove("b");

    auto results = db.scan("a", "z");

    REQUIRE(results == std::vector<std::pair<std::string, std::string>>{{"a", "1"}, {"c", "3"}});
}

TEST_CASE("no SSTables exist until the flush threshold is crossed", "[database]") {
    TempDir dir;
    Database db(dir.path, kTinyFlushThreshold);

    db.put("a", "1");
    REQUIRE(db.sstable_count() == 0);
}

TEST_CASE("exceeding the flush threshold writes an SSTable and starts a fresh MemTable", "[database]") {
    TempDir dir;
    Database db(dir.path, kTinyFlushThreshold);

    for (int i = 0; i < 10; ++i) {
        db.put("key-" + std::to_string(i), "some-reasonably-long-value-" + std::to_string(i));
    }

    REQUIRE(db.sstable_count() >= 1);
    // Data written before the flush must still be readable afterward -
    // this is the "read falls through to an SSTable" case.
    REQUIRE(db.get("key-0") == "some-reasonably-long-value-0");
}

TEST_CASE("a read for a key that only exists in a flushed SSTable falls through correctly",
          "[database]") {
    TempDir dir;
    Database db(dir.path, kTinyFlushThreshold);

    db.put("only-in-sstable", "flushed-value");
    for (int i = 0; i < 5; ++i) {
        // Push the MemTable over the threshold with unrelated writes so
        // the first key is no longer in the (now fresh) MemTable at all.
        db.put("filler-" + std::to_string(i), "padding-to-force-a-flush-" + std::to_string(i));
    }
    REQUIRE(db.sstable_count() >= 1);

    REQUIRE(db.get("only-in-sstable") == "flushed-value");
}

TEST_CASE("a tombstone in the MemTable shadows a live value in an older SSTable", "[database]") {
    TempDir dir;
    Database db(dir.path, kTinyFlushThreshold);

    db.put("key", "original");
    for (int i = 0; i < 5; ++i) {
        db.put("filler-" + std::to_string(i), "padding-to-force-a-flush-" + std::to_string(i));
    }
    REQUIRE(db.sstable_count() >= 1);  // "key" is now only in an SSTable

    db.remove("key");  // tombstone lands in the fresh MemTable

    REQUIRE(db.get("key") == std::nullopt);
}

TEST_CASE("a newer SSTable's value wins over an older SSTable for the same key", "[database]") {
    TempDir dir;
    Database db(dir.path, kTinyFlushThreshold);

    db.put("key", "old-value");
    for (int i = 0; i < 5; ++i) {
        db.put("filler-a-" + std::to_string(i), "padding-to-force-a-flush-" + std::to_string(i));
    }
    const size_t sstables_after_first_flush = db.sstable_count();
    REQUIRE(sstables_after_first_flush >= 1);

    db.put("key", "new-value");  // overwrite, lands in the new MemTable
    for (int i = 0; i < 5; ++i) {
        db.put("filler-b-" + std::to_string(i), "padding-to-force-a-flush-" + std::to_string(i));
    }
    REQUIRE(db.sstable_count() > sstables_after_first_flush);  // a second flush happened

    REQUIRE(db.get("key") == "new-value");
}

TEST_CASE("scan merges live MemTable and SSTable entries, newest winning, tombstones dropped",
          "[database]") {
    TempDir dir;
    Database db(dir.path, kTinyFlushThreshold);

    db.put("a", "flushed-a");
    db.put("b", "flushed-b-old");
    db.put("c", "flushed-c-deleted");
    for (int i = 0; i < 5; ++i) {
        db.put("filler-" + std::to_string(i), "padding-to-force-a-flush-" + std::to_string(i));
    }
    REQUIRE(db.sstable_count() >= 1);

    db.put("b", "memtable-b-new");  // overwrite in the fresh MemTable
    db.remove("c");                 // tombstone in the fresh MemTable
    db.put("d", "memtable-d");      // never touched an SSTable at all

    // Scoped to exclude the "filler-*" keys used above to force a flush -
    // this test is about a/b/c/d specifically, not everything in the DB.
    auto results = db.scan("a", "e");

    REQUIRE(results == std::vector<std::pair<std::string, std::string>>{
                            {"a", "flushed-a"},
                            {"b", "memtable-b-new"},
                            {"d", "memtable-d"},
                        });
}

TEST_CASE("data survives closing and reopening the Database - simulated restart", "[database]") {
    TempDir dir;

    {
        Database db(dir.path, kTinyFlushThreshold);
        db.put("flushed-key", "flushed-value");
        for (int i = 0; i < 5; ++i) {
            db.put("filler-" + std::to_string(i), "padding-to-force-a-flush-" + std::to_string(i));
        }
        REQUIRE(db.sstable_count() >= 1);
        db.put("unflushed-key", "unflushed-value");  // stays in the WAL only
    }  // Database destroyed here, as if the process restarted

    Database reopened(dir.path, kTinyFlushThreshold);
    REQUIRE(reopened.sstable_count() >= 1);          // SSTables rediscovered from disk
    REQUIRE(reopened.get("flushed-key") == "flushed-value");
    REQUIRE(reopened.get("unflushed-key") == "unflushed-value");  // recovered via WAL replay
}

TEST_CASE("a delete survives closing and reopening the Database", "[database]") {
    TempDir dir;

    {
        Database db(dir.path);
        db.put("key", "value");
        db.remove("key");
    }

    Database reopened(dir.path);
    REQUIRE(reopened.get("key") == std::nullopt);
}

// --- Compaction ---

TEST_CASE("WaitForBackgroundCompaction returns immediately when nothing was triggered",
          "[database][compaction]") {
    TempDir dir;
    Database db(dir.path, kTinyFlushThreshold, kCompactAtThree);
    db.WaitForBackgroundCompaction();  // must not hang
    SUCCEED();
}

TEST_CASE("crossing the compaction trigger count merges SSTables down to one",
          "[database][compaction]") {
    TempDir dir;
    Database db(dir.path, kFlushEveryWrite, kCompactAtThree);

    db.put("a", "1");
    db.put("b", "2");
    db.put("c", "3");
    db.WaitForBackgroundCompaction();

    REQUIRE(db.sstable_count() == 1);
    REQUIRE(db.get("a") == "1");
    REQUIRE(db.get("b") == "2");
    REQUIRE(db.get("c") == "3");
}

TEST_CASE("compaction drops the older value for an overwritten key, at the storage level",
          "[database][compaction]") {
    TempDir dir;
    Database db(dir.path, kFlushEveryWrite, kCompactAtThree);

    db.put("key", "old-value");
    db.put("key", "new-value");
    db.put("filler", "unrelated");  // crosses the trigger count
    db.WaitForBackgroundCompaction();
    REQUIRE(db.sstable_count() == 1);

    // Inspect the compacted SSTable directly - if the old value merely
    // lost to the new one at read time, that wouldn't prove it's gone
    // from disk; AllRaw() over the single remaining file does.
    auto entries = FindSingleSSTable(dir.path).AllRaw();
    REQUIRE(entries.size() == 2);  // "key" once, "filler" once
    for (const auto& [key, entry] : entries) {
        if (key == "key") {
            REQUIRE(entry.value == "new-value");
        }
    }
}

TEST_CASE("compaction drops a tombstone once nothing older can shadow it",
          "[database][compaction]") {
    TempDir dir;
    Database db(dir.path, kFlushEveryWrite, kCompactAtThree);

    db.put("key", "value");
    db.remove("key");
    db.put("filler", "unrelated");  // crosses the trigger count
    db.WaitForBackgroundCompaction();
    REQUIRE(db.sstable_count() == 1);

    auto entries = FindSingleSSTable(dir.path).AllRaw();
    for (const auto& [key, entry] : entries) {
        REQUIRE(key != "key");  // tombstone and everything it shadowed are both gone
    }
    REQUIRE(db.get("key") == std::nullopt);  // still correctly absent through the API
}

TEST_CASE("compaction across more than two generations keeps only the newest value",
          "[database][compaction]") {
    TempDir dir;
    Database db(dir.path, kFlushEveryWrite, kCompactAtThree);

    db.put("key", "v1");
    db.put("key", "v2");
    db.put("key", "v3");
    db.WaitForBackgroundCompaction();

    REQUIRE(db.sstable_count() == 1);
    REQUIRE(db.get("key") == "v3");
}

TEST_CASE("compacted data survives closing and reopening the Database", "[database][compaction]") {
    TempDir dir;
    {
        Database db(dir.path, kFlushEveryWrite, kCompactAtThree);
        db.put("a", "1");
        db.put("b", "2");
        db.put("c", "3");
        db.WaitForBackgroundCompaction();
        REQUIRE(db.sstable_count() == 1);
    }

    Database reopened(dir.path, kFlushEveryWrite, kCompactAtThree);
    REQUIRE(reopened.sstable_count() == 1);
    REQUIRE(reopened.get("a") == "1");
    REQUIRE(reopened.get("b") == "2");
    REQUIRE(reopened.get("c") == "3");
}

TEST_CASE("a crash between the compacted file's rename and the old files' deletion self-heals",
          "[database][compaction]") {
    // Simulates the exact window CompactOnce() can die in: the merged
    // SSTable is already durably renamed into place, but the SSTables it
    // replaces haven't been deleted yet. A fresh Database should just
    // treat the highest-numbered file as authoritative and read
    // correctly, with the (now redundant) old files self-healing away on
    // the next real compaction rather than causing any incorrect read.
    TempDir dir;

    WriteSSTable(dir.path / "sst-000001.sst", {{"key", Entry{"stale-1", false}}});
    WriteSSTable(dir.path / "sst-000002.sst", {{"key", Entry{"stale-2", false}}});
    WriteSSTable(dir.path / "sst-000003.sst", {{"key", Entry{"fresh", false}}});

    Database db(dir.path);

    REQUIRE(db.sstable_count() == 3);  // all three rediscovered, none silently dropped
    REQUIRE(db.get("key") == "fresh");  // highest generation number wins
}

TEST_CASE("concurrent puts and gets survive background compaction without data loss or a crash",
          "[database][compaction]") {
    TempDir dir;
    Database db(dir.path, kFlushEveryWrite, kCompactAtThree);

    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 200;
    std::atomic<bool> failed{false};

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&db, t, &failed] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                const std::string key = "t" + std::to_string(t) + "-k" + std::to_string(i);
                db.put(key, "value");
                if (db.get(key) != "value") {
                    failed = true;
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    db.WaitForBackgroundCompaction();

    REQUIRE_FALSE(failed);
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kOpsPerThread; ++i) {
            const std::string key = "t" + std::to_string(t) + "-k" + std::to_string(i);
            REQUIRE(db.get(key) == "value");
        }
    }
}
