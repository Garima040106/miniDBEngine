#pragma once

#include "tinylsm/kv_store.h"
#include "tinylsm/memtable.h"

namespace tinylsm {

// The engine callers actually talk to. Right now this is a thin
// pass-through to a single MemTable - no WAL, no flushing to SSTables -
// but it's the seam where those get added later without changing the
// public KVStore interface above it.
class Database : public KVStore {
public:
    void put(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) const override;
    void remove(const std::string& key) override;
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start, const std::string& end) const override;

private:
    MemTable memtable_;
};

}  // namespace tinylsm
