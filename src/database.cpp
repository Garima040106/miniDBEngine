#include "tinylsm/database.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <optional>
#include <string_view>
#include <system_error>

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

Database::Database(std::filesystem::path db_dir, size_t flush_threshold_bytes,
                    size_t compaction_trigger_count)
    : db_dir_(std::move(db_dir)),
      flush_threshold_bytes_(flush_threshold_bytes),
      compaction_trigger_count_(compaction_trigger_count) {
    std::filesystem::create_directories(db_dir_);

    // Leftover "*.sst.tmp" files mean a flush or compaction was
    // interrupted before its rename - by construction (see CLAUDE.md)
    // that means the data they'd have contained is still safe elsewhere
    // (the WAL, or the SSTables being compacted), so these are always
    // safe to discard. Pure hygiene, not required for correctness.
    for (const auto& dir_entry : std::filesystem::directory_iterator(db_dir_)) {
        if (dir_entry.path().extension() == ".tmp") {
            std::error_code ec;
            std::filesystem::remove(dir_entry.path(), ec);
        }
    }

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
        sstables_.push_back(std::make_shared<SSTableReader>(db_dir_ / SSTableFilename(number)));
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

    compaction_thread_ = std::thread(&Database::CompactionThreadMain, this);
}

Database::~Database() {
    {
        std::lock_guard<std::mutex> lock(compaction_cv_mutex_);
        shutting_down_ = true;
    }
    compaction_cv_.notify_one();
    compaction_thread_.join();
}

void Database::put(const std::string& key, const std::string& value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    wal_->Append(RecordType::kPut, key, value);
    memtable_.put(key, value);
    MaybeFlush();
}

void Database::remove(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    wal_->Append(RecordType::kDelete, key, "");
    memtable_.remove(key);
    MaybeFlush();
}

std::optional<std::string> Database::get(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

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
    std::shared_lock<std::shared_mutex> lock(mutex_);

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

size_t Database::sstable_count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return sstables_.size();
}

void Database::WaitForBackgroundCompaction() const {
    std::unique_lock<std::mutex> lock(compaction_cv_mutex_);
    compaction_cv_.wait(lock, [this] { return !compaction_requested_ && !compaction_running_; });
}

void Database::MaybeFlush() {
    if (memtable_.approximate_size_bytes() >= flush_threshold_bytes_) {
        Flush();
    }
}

void Database::Flush() {
    const std::filesystem::path sstable_path = db_dir_ / SSTableFilename(next_sstable_number_);
    WriteSSTable(sstable_path, memtable_.entries());
    sstables_.push_back(std::make_shared<SSTableReader>(sstable_path));
    ++next_sstable_number_;

    // Only safe now that the new SSTable is durably on disk (and open):
    // the data that backed this MemTable generation no longer needs the
    // WAL to survive a crash. See CLAUDE.md's "SSTable" section for why
    // this exact ordering (SSTable durable and visible, *then* WAL
    // reset) is what makes a flush crash-safe.
    wal_->Reset();
    memtable_ = MemTable{};

    MaybeTriggerCompaction();
}

void Database::MaybeTriggerCompaction() {
    if (sstables_.size() < compaction_trigger_count_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(compaction_cv_mutex_);
        compaction_requested_ = true;
    }
    compaction_cv_.notify_one();
}

void Database::CompactionThreadMain() {
    while (true) {
        std::unique_lock<std::mutex> lock(compaction_cv_mutex_);
        compaction_cv_.wait(lock, [this] { return compaction_requested_ || shutting_down_; });
        if (shutting_down_) {
            return;
        }
        compaction_requested_ = false;
        compaction_running_ = true;
        lock.unlock();

        CompactOnce();

        lock.lock();
        compaction_running_ = false;
        lock.unlock();
        compaction_cv_.notify_all();  // wake any WaitForBackgroundCompaction() callers
    }
}

void Database::CompactOnce() {
    std::vector<std::shared_ptr<SSTableReader>> to_compact;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (sstables_.size() < 2) {
            return;  // nothing meaningful to merge
        }
        to_compact = sstables_;  // copy of shared_ptrs - cheap, safe
    }

    // Everything below runs without mutex_ held, so foreground put()/
    // get()/scan() proceed freely while this (potentially slow) work
    // happens - see mutex_'s doc comment in database.h.
    std::map<std::string, Entry> merged = MergeSSTables(to_compact);

    // This is a *full* compaction of everything present at snapshot
    // time - nothing older than `to_compact` exists outside it - so any
    // tombstone that won the merge can be dropped for good rather than
    // written to the compacted output. See CLAUDE.md's "Compaction"
    // section for why that's specifically true here.
    for (auto it = merged.begin(); it != merged.end();) {
        if (it->second.is_tombstone) {
            it = merged.erase(it);
        } else {
            ++it;
        }
    }

    uint64_t compacted_number = 0;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        compacted_number = next_sstable_number_++;
    }
    const std::filesystem::path compacted_path = db_dir_ / SSTableFilename(compacted_number);

    // Same temp-file-then-rename pattern as Flush() - see CLAUDE.md's
    // "Compaction" section for the crash-safety argument this depends on.
    WriteSSTable(compacted_path, merged);
    auto compacted_reader = std::make_shared<SSTableReader>(compacted_path);

    std::vector<std::filesystem::path> old_paths;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        // sstables_ only ever grows by appending (Flush()) or gets
        // replaced by a compaction (this function) - and only one
        // compaction runs at a time (a single background thread), so
        // the prefix snapshotted into `to_compact` above is still
        // exactly sstables_[0, to_compact.size()) here, even though
        // concurrent flushes may have appended more after it.
        old_paths.reserve(to_compact.size());
        for (size_t i = 0; i < to_compact.size(); ++i) {
            old_paths.push_back(sstables_[i]->path());
        }

        std::vector<std::shared_ptr<SSTableReader>> new_sstables;
        new_sstables.reserve(1 + sstables_.size() - to_compact.size());
        new_sstables.push_back(compacted_reader);
        for (size_t i = to_compact.size(); i < sstables_.size(); ++i) {
            new_sstables.push_back(sstables_[i]);  // flushed concurrently - preserved, not compacted
        }
        sstables_ = std::move(new_sstables);
        // ^ This single assignment is the entire swap: any concurrent
        // get()/scan() holding (or waiting for) the shared lock sees
        // either the fully-old list or this fully-new one, never a mix.
    }

    // Only delete the old files now that the merged SSTable is durably
    // on disk *and* no longer referenced from sstables_ by anything
    // that still needs the originals. Best-effort: if this fails or the
    // process dies here, the old files just get rediscovered on the
    // next startup alongside the (newer-numbered, so still correctly
    // shadowing them) merged file - redundant but not incorrect, and
    // self-healing on the next compaction. See CLAUDE.md.
    for (const auto& path : old_paths) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

}  // namespace tinylsm
