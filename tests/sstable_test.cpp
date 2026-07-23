#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <map>
#include <string>

#include "tinylsm/sstable.h"
#include "test_util.h"

using tinylsm::Entry;
using tinylsm::SSTableReader;
using tinylsm::WriteSSTable;
using tinylsm::test::TempDir;

namespace {

std::map<std::string, Entry> MakeEntries(int count) {
    std::map<std::string, Entry> entries;
    for (int i = 0; i < count; ++i) {
        // Zero-padded so lexical (std::map) order matches numeric order.
        char key[16];
        std::snprintf(key, sizeof(key), "key-%04d", i);
        entries[key] = Entry{"value-" + std::to_string(i), false};
    }
    return entries;
}

}  // namespace

TEST_CASE("a written SSTable can be read back for every key", "[sstable]") {
    TempDir dir;
    std::map<std::string, Entry> entries;
    entries["a"] = Entry{"1", false};
    entries["b"] = Entry{"2", false};
    entries["c"] = Entry{"3", false};

    WriteSSTable(dir.path / "table.sst", entries);
    SSTableReader reader(dir.path / "table.sst");

    auto a = reader.Find("a");
    REQUIRE(a.has_value());
    REQUIRE(a->value == "1");
    REQUIRE_FALSE(a->is_tombstone);

    REQUIRE(reader.Find("c")->value == "3");
}

TEST_CASE("Find returns nullopt for a key that was never in the table", "[sstable]") {
    TempDir dir;
    std::map<std::string, Entry> entries{{"b", Entry{"2", false}}};
    WriteSSTable(dir.path / "table.sst", entries);
    SSTableReader reader(dir.path / "table.sst");

    REQUIRE(reader.Find("missing") == std::nullopt);
}

TEST_CASE("Find returns nullopt for keys sorting before or after everything", "[sstable]") {
    TempDir dir;
    std::map<std::string, Entry> entries{{"m", Entry{"mid", false}}};
    WriteSSTable(dir.path / "table.sst", entries);
    SSTableReader reader(dir.path / "table.sst");

    REQUIRE(reader.Find("a") == std::nullopt);  // sorts before the only key
    REQUIRE(reader.Find("z") == std::nullopt);  // sorts after the only key
}

TEST_CASE("tombstones round-trip through a write and read", "[sstable]") {
    TempDir dir;
    std::map<std::string, Entry> entries;
    entries["deleted"] = Entry{"", true};
    WriteSSTable(dir.path / "table.sst", entries);
    SSTableReader reader(dir.path / "table.sst");

    auto found = reader.Find("deleted");
    REQUIRE(found.has_value());
    REQUIRE(found->is_tombstone);
}

TEST_CASE("an empty MemTable produces a valid, empty SSTable", "[sstable]") {
    TempDir dir;
    WriteSSTable(dir.path / "table.sst", {});
    SSTableReader reader(dir.path / "table.sst");

    REQUIRE(reader.Find("anything") == std::nullopt);
    REQUIRE(reader.ScanRaw("a", "z").empty());
}

TEST_CASE("WriteSSTable leaves no temp file behind and the target file exists", "[sstable]") {
    TempDir dir;
    std::filesystem::path target = dir.path / "table.sst";
    WriteSSTable(target, {{"a", Entry{"1", false}}});

    REQUIRE(std::filesystem::exists(target));
    REQUIRE_FALSE(std::filesystem::exists(dir.path / "table.sst.tmp"));
}

TEST_CASE("opening a file with a bad magic number throws", "[sstable]") {
    TempDir dir;
    std::filesystem::path bad_path = dir.path / "not-an-sstable.sst";
    {
        std::ofstream out(bad_path, std::ios::binary);
        out << std::string(64, '\x00');  // long enough for a footer, but garbage
    }

    REQUIRE_THROWS_AS(SSTableReader(bad_path), std::runtime_error);
}

TEST_CASE("a file too small to hold a footer throws", "[sstable]") {
    TempDir dir;
    std::filesystem::path short_path = dir.path / "too-short.sst";
    {
        std::ofstream out(short_path, std::ios::binary);
        out << "tiny";
    }

    REQUIRE_THROWS_AS(SSTableReader(short_path), std::runtime_error);
}

TEST_CASE("point lookups work across multiple sparse index blocks", "[sstable]") {
    // The sparse index samples every 16th entry - 50 entries spans
    // several blocks, exercising the "step back to the right block"
    // logic beyond just the first one.
    TempDir dir;
    auto entries = MakeEntries(50);
    WriteSSTable(dir.path / "table.sst", entries);
    SSTableReader reader(dir.path / "table.sst");

    for (const auto& [key, entry] : entries) {
        auto found = reader.Find(key);
        REQUIRE(found.has_value());
        REQUIRE(found->value == entry.value);
    }
    REQUIRE(reader.Find("key-9999") == std::nullopt);
}

TEST_CASE("ScanRaw returns the full sorted range across multiple blocks", "[sstable]") {
    TempDir dir;
    auto entries = MakeEntries(50);
    WriteSSTable(dir.path / "table.sst", entries);
    SSTableReader reader(dir.path / "table.sst");

    auto results = reader.ScanRaw("key-0000", "key-9999");

    REQUIRE(results.size() == 50);
    REQUIRE(results.front().first == "key-0000");
    REQUIRE(results.back().first == "key-0049");
    for (size_t i = 1; i < results.size(); ++i) {
        REQUIRE(results[i - 1].first < results[i].first);
    }
}

TEST_CASE("ScanRaw bounds a range that starts and ends mid-block", "[sstable]") {
    TempDir dir;
    auto entries = MakeEntries(50);
    WriteSSTable(dir.path / "table.sst", entries);
    SSTableReader reader(dir.path / "table.sst");

    // key-0018 through key-0022 - inside block boundaries (16-31), not
    // aligned to a sampled index key (0016).
    auto results = reader.ScanRaw("key-0018", "key-0023");

    REQUIRE(results.size() == 5);
    REQUIRE(results.front().first == "key-0018");
    REQUIRE(results.back().first == "key-0022");
}

TEST_CASE("ScanRaw over an empty table returns nothing", "[sstable]") {
    TempDir dir;
    WriteSSTable(dir.path / "table.sst", {});
    SSTableReader reader(dir.path / "table.sst");

    REQUIRE(reader.ScanRaw("a", "z").empty());
}
