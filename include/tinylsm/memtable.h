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
// caller (Database) rather than hiding them, since the SSTable reader
// needs the same distinction.
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

    // Every entry in sorted key order, tombstones included - used by
    // Database when flushing this MemTable out to an SSTable. Exposing
    // the backing map directly is a small, deliberate leak of the
    // internal representation, in exchange for not inventing a custom
    // iterator type for a single internal use.
    const std::map<std::string, Entry>& entries() const { return table_; }

    // Sum of every live entry's key+value size, updated incrementally as
    // entries are inserted/overwritten/tombstoned. This is what Database
    // compares against the flush threshold - approximate on purpose
    // (it ignores std::map's own per-node overhead), which is normal for
    // this kind of size accounting and fine for deciding "roughly when"
    // to flush.
    size_t approximate_size_bytes() const { return approximate_size_bytes_; }

private:
    void Upsert(const std::string& key, Entry entry);

    std::map<std::string, Entry> table_;
    size_t approximate_size_bytes_ = 0;
};

}  // namespace tinylsm
