#include "tinylsm/memtable.h"

#include <utility>

namespace tinylsm {

namespace {

size_t EntrySize(const std::string& key, const Entry& entry) { return key.size() + entry.value.size(); }

}  // namespace

void MemTable::put(const std::string& key, std::string value) {
    Upsert(key, Entry{std::move(value), false});
}

void MemTable::remove(const std::string& key) { Upsert(key, Entry{std::string{}, true}); }

void MemTable::Upsert(const std::string& key, Entry entry) {
    auto it = table_.find(key);
    if (it != table_.end()) {
        approximate_size_bytes_ -= EntrySize(key, it->second);
        it->second = std::move(entry);
    } else {
        it = table_.emplace(key, std::move(entry)).first;
    }
    approximate_size_bytes_ += EntrySize(key, it->second);
}

std::optional<std::string> MemTable::get(const std::string& key) const {
    const Entry* entry = find(key);
    if (entry == nullptr || entry->is_tombstone) {
        return std::nullopt;
    }
    return entry->value;
}

const Entry* MemTable::find(const std::string& key) const {
    auto it = table_.find(key);
    if (it == table_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::vector<std::pair<std::string, std::string>> MemTable::scan(const std::string& start,
                                                                  const std::string& end) const {
    std::vector<std::pair<std::string, std::string>> results;
    for (auto it = table_.lower_bound(start); it != table_.end() && it->first < end; ++it) {
        if (!it->second.is_tombstone) {
            results.emplace_back(it->first, it->second.value);
        }
    }
    return results;
}

}  // namespace tinylsm
