#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

#include "tinylsm/memtable.h"

using tinylsm::MemTable;

TEST_CASE("a key that was never written is absent", "[memtable]") {
    MemTable table;
    REQUIRE(table.get("missing") == std::nullopt);
    REQUIRE(table.find("missing") == nullptr);
    REQUIRE(table.empty());
}

TEST_CASE("put then get returns the value", "[memtable]") {
    MemTable table;
    table.put("a", "1");
    REQUIRE(table.get("a") == "1");
    REQUIRE(table.size() == 1);
}

TEST_CASE("a later put overwrites an earlier one for the same key", "[memtable]") {
    MemTable table;
    table.put("a", "1");
    table.put("a", "2");
    REQUIRE(table.get("a") == "2");
    REQUIRE(table.size() == 1);
}

TEST_CASE("remove makes a previously-present key read as absent", "[memtable]") {
    MemTable table;
    table.put("a", "1");
    table.remove("a");
    REQUIRE(table.get("a") == std::nullopt);
}

TEST_CASE("remove on a key that was never present still records a tombstone", "[memtable]") {
    MemTable table;
    table.remove("a");

    REQUIRE(table.get("a") == std::nullopt);
    const auto* entry = table.find("a");
    REQUIRE(entry != nullptr);
    REQUIRE(entry->is_tombstone);
}

TEST_CASE("a put after a remove makes the key live again", "[memtable]") {
    MemTable table;
    table.put("a", "1");
    table.remove("a");
    table.put("a", "2");
    REQUIRE(table.get("a") == "2");
}

TEST_CASE("scan returns live entries in sorted key order within a half-open range", "[memtable]") {
    MemTable table;
    table.put("b", "2");
    table.put("a", "1");
    table.put("d", "4");
    table.put("c", "3");

    auto results = table.scan("a", "d");

    REQUIRE(results ==
            std::vector<std::pair<std::string, std::string>>{{"a", "1"}, {"b", "2"}, {"c", "3"}});
}

TEST_CASE("scan excludes tombstoned keys", "[memtable]") {
    MemTable table;
    table.put("a", "1");
    table.put("b", "2");
    table.remove("a");

    auto results = table.scan("a", "z");

    REQUIRE(results == std::vector<std::pair<std::string, std::string>>{{"b", "2"}});
}

TEST_CASE("scan start is inclusive and end is exclusive", "[memtable]") {
    MemTable table;
    table.put("a", "1");
    table.put("b", "2");
    table.put("c", "3");

    auto results = table.scan("b", "c");

    REQUIRE(results == std::vector<std::pair<std::string, std::string>>{{"b", "2"}});
}

TEST_CASE("scan over an empty table returns nothing", "[memtable]") {
    MemTable table;
    REQUIRE(table.scan("a", "z").empty());
}

TEST_CASE("scan with no matching keys returns nothing", "[memtable]") {
    MemTable table;
    table.put("x", "1");
    REQUIRE(table.scan("a", "b").empty());
}

TEST_CASE("approximate_size_bytes starts at zero and grows with puts", "[memtable]") {
    MemTable table;
    REQUIRE(table.approximate_size_bytes() == 0);

    table.put("ab", "xyz");  // 2 + 3 bytes
    REQUIRE(table.approximate_size_bytes() == 5);

    table.put("k", "v");  // + 1 + 1 bytes
    REQUIRE(table.approximate_size_bytes() == 7);
}

TEST_CASE("overwriting a key adjusts size by the delta, not by adding the new size", "[memtable]") {
    MemTable table;
    table.put("key", "short");       // 3 + 5 = 8
    table.put("key", "much-longer");  // replaces, not adds: 3 + 11 = 14

    REQUIRE(table.approximate_size_bytes() == 14);
}

TEST_CASE("remove replaces a live entry's size with the (smaller) tombstone size", "[memtable]") {
    MemTable table;
    table.put("key", "some-value");  // 3 + 10 = 13
    table.remove("key");             // tombstone: 3 + 0 = 3

    REQUIRE(table.approximate_size_bytes() == 3);
}

TEST_CASE("entries() reflects the same sorted, tombstone-inclusive contents as the table", "[memtable]") {
    MemTable table;
    table.put("b", "2");
    table.put("a", "1");
    table.remove("b");

    const auto& entries = table.entries();

    REQUIRE(entries.size() == 2);
    REQUIRE(entries.at("a").value == "1");
    REQUIRE_FALSE(entries.at("a").is_tombstone);
    REQUIRE(entries.at("b").is_tombstone);
}
