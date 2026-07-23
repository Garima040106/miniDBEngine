#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tinylsm/database.h"
#include "tinylsm/kv_store.h"
#include "test_util.h"

using tinylsm::Database;
using tinylsm::KVStore;
using tinylsm::test::TempDir;

namespace {
// Small enough that a couple of short puts push a MemTable over it,
// so tests can force a flush without writing megabytes of data.
constexpr size_t kTinyFlushThreshold = 32;
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
