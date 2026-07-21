#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tinylsm/database.h"
#include "tinylsm/kv_store.h"

using tinylsm::Database;
using tinylsm::KVStore;

TEST_CASE("Database GET on a never-written key returns nullopt", "[database]") {
    Database db;
    REQUIRE(db.get("nope") == std::nullopt);
}

TEST_CASE("Database put/get/remove round-trip through the KVStore interface", "[database]") {
    std::unique_ptr<KVStore> store = std::make_unique<Database>();

    store->put("key", "value");
    REQUIRE(store->get("key") == "value");

    store->remove("key");
    REQUIRE(store->get("key") == std::nullopt);
}

TEST_CASE("Database scan reflects the current state and excludes tombstones", "[database]") {
    Database db;
    db.put("a", "1");
    db.put("b", "2");
    db.put("c", "3");
    db.remove("b");

    auto results = db.scan("a", "z");

    REQUIRE(results == std::vector<std::pair<std::string, std::string>>{{"a", "1"}, {"c", "3"}});
}
