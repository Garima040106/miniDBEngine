#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tinylsm {

// One in-memory sorted entry: either a real value, or a tombstone
// marking that the key was deleted. See CLAUDE.md's "Tombstones" section
// for why a delete can't just erase the key outright.
struct Entry {
    std::string value;
    bool is_tombstone = false;
};

// A sorted in-memory table of the most recent write for every key.
// std::map keeps entries in key order for free, which scan() relies on.
// Not itself a KVStore - it deliberately exposes tombstones to its
// caller (Database) rather than hiding them, since a future SSTable
// reader will need the same distinction.
class MemTable {
public:
    void put(const std::string& key, std::string value);
    void remove(const std::string& key);

    // nullopt if the key isn't present *or* is tombstoned - callers that
    // need to distinguish those two cases should use find() instead.
    std::optional<std::string> get(const std::string& key) const;

    // The raw entry (value or tombstone), if this key has ever been
    // written in this MemTable at all; nullptr otherwise.
    const Entry* find(const std::string& key) const;

    // Every live (non-tombstone) entry with start <= key < end.
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start, const std::string& end) const;

    size_t size() const { return table_.size(); }
    bool empty() const { return table_.empty(); }

private:
    std::map<std::string, Entry> table_;
};

}  // namespace tinylsm
