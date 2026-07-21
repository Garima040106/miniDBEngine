#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tinylsm {

// The store's public contract. Every concrete backing (today: an
// in-memory MemTable only; later: MemTable + WAL + SSTables) implements
// this same interface, so callers never need to know which one they have.
class KVStore {
public:
    virtual ~KVStore() = default;

    virtual void put(const std::string& key, const std::string& value) = 0;

    // nullopt if the key was never written, or was deleted.
    virtual std::optional<std::string> get(const std::string& key) const = 0;

    virtual void remove(const std::string& key) = 0;

    // Every live entry with start <= key < end, in ascending key order.
    virtual std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start, const std::string& end) const = 0;
};

}  // namespace tinylsm
