#include "tinylsm/database.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <optional>
#include <string_view>

namespace tinylsm {

namespace {

std::string SSTableFilename(uint64_t number) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "sst-%06llu.sst", static_cast<unsigned long long>(number));
    return buf;
}

// nullopt if `filename` doesn't match "sst-<digits>.sst".
std::optional<uint64_t> ParseSSTableNumber(const std::string& filename) {
    static constexpr std::string_view kPrefix = "sst-";
    static constexpr std::string_view kSuffix = ".sst";
    if (filename.size() <= kPrefix.size() + kSuffix.size()) {
        return std::nullopt;
    }
    if (filename.compare(0, kPrefix.size(), kPrefix) != 0) {
        return std::nullopt;
    }
    if (filename.compare(filename.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return std::nullopt;
    }

    const std::string digits =
        filename.substr(kPrefix.size(), filename.size() - kPrefix.size() - kSuffix.size());
    const bool all_digits = !digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c));
    });
    if (!all_digits) {
        return std::nullopt;
    }
    return std::stoull(digits);
}

}  // namespace

Database::Database(std::filesystem::path db_dir, size_t flush_threshold_bytes)
    : db_dir_(std::move(db_dir)), flush_threshold_bytes_(flush_threshold_bytes) {
    std::filesystem::create_directories(db_dir_);

    // SSTables are immutable and self-contained, so rediscovering them
    // is just: list the directory, sort by generation number, reopen
    // each - oldest first, so sstables_.back() is always the newest.
    std::vector<uint64_t> numbers;
    for (const auto& dir_entry : std::filesystem::directory_iterator(db_dir_)) {
        if (auto number = ParseSSTableNumber(dir_entry.path().filename().string())) {
            numbers.push_back(*number);
        }
    }
    std::sort(numbers.begin(), numbers.end());
    for (uint64_t number : numbers) {
        sstables_.push_back(std::make_unique<SSTableReader>(db_dir_ / SSTableFilename(number)));
        next_sstable_number_ = number + 1;
    }

    // The WAL only ever holds whatever wasn't flushed yet (it's Reset()
    // right after each successful flush) - replaying it into a fresh
    // MemTable recovers anything acknowledged since then.
    wal_ = std::make_unique<WriteAheadLog>(db_dir_ / "wal.log");
    wal_->Replay([this](RecordType type, const std::string& key, const std::string& value) {
        if (type == RecordType::kPut) {
            memtable_.put(key, value);
        } else {
            memtable_.remove(key);
        }
    });
}

void Database::put(const std::string& key, const std::string& value) {
    wal_->Append(RecordType::kPut, key, value);
    memtable_.put(key, value);
    MaybeFlush();
}

void Database::remove(const std::string& key) {
    wal_->Append(RecordType::kDelete, key, "");
    memtable_.remove(key);
    MaybeFlush();
}

std::optional<std::string> Database::get(const std::string& key) const {
    if (const Entry* entry = memtable_.find(key)) {
        return entry->is_tombstone ? std::nullopt : std::make_optional(entry->value);
    }

    // Newest to oldest: the first SSTable with any entry for this key -
    // live value or tombstone - has the answer. Older SSTables further
    // back may have a stale value for the same key; that's exactly what
    // "newest wins" means, and it's why this loop returns on the first
    // hit rather than continuing.
    for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
        if (auto entry = (*it)->Find(key)) {
            return entry->is_tombstone ? std::nullopt : std::make_optional(entry->value);
        }
    }

    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> Database::scan(const std::string& start,
                                                                  const std::string& end) const {
    // Merge oldest-to-newest into one map so a later write for the same
    // key always overwrites an earlier one - the MemTable is applied
    // last since it's the newest generation.
    std::map<std::string, Entry> merged;

    for (const auto& sstable : sstables_) {
        for (auto& [key, entry] : sstable->ScanRaw(start, end)) {
            merged[key] = entry;
        }
    }
    for (auto it = memtable_.entries().lower_bound(start);
         it != memtable_.entries().end() && it->first < end; ++it) {
        merged[it->first] = it->second;
    }

    std::vector<std::pair<std::string, std::string>> results;
    for (auto& [key, entry] : merged) {
        if (!entry.is_tombstone) {
            results.emplace_back(key, entry.value);
        }
    }
    return results;
}

void Database::MaybeFlush() {
    if (memtable_.approximate_size_bytes() >= flush_threshold_bytes_) {
        Flush();
    }
}

void Database::Flush() {
    const std::filesystem::path sstable_path = db_dir_ / SSTableFilename(next_sstable_number_);
    WriteSSTable(sstable_path, memtable_.entries());
    sstables_.push_back(std::make_unique<SSTableReader>(sstable_path));
    ++next_sstable_number_;

    // Only safe now that the new SSTable is durably on disk (and open):
    // the data that backed this MemTable generation no longer needs the
    // WAL to survive a crash. See CLAUDE.md's "SSTable" section for why
    // this exact ordering (SSTable durable and visible, *then* WAL
    // reset) is what makes a flush crash-safe.
    wal_->Reset();
    memtable_ = MemTable{};
}

}  // namespace tinylsm
