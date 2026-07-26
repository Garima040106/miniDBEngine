#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "tinylsm/kv_store.h"
#include "tinylsm/memtable.h"
#include "tinylsm/sstable.h"
#include "tinylsm/wal.h"

namespace tinylsm {

// The engine callers actually talk to: a MemTable backed by a WAL for
// durability, with older generations preserved as immutable, on-disk
// SSTables once a MemTable is flushed, periodically merged back down to
// one by a background compaction thread. See CLAUDE.md's "Read path" /
// "Write path" / "SSTable" / "Compaction" sections for exactly how a
// request is resolved across these layers, the on-disk crash-safety
// argument, and the locking strategy explained next to `mutex_` below.
class Database : public KVStore {
public:
    // A MemTable is flushed to a new SSTable once its approximate size
    // reaches this many bytes. The default is sized for the REPL/real
    // use; tests pass a much smaller value so a flush can be triggered
    // deterministically without writing megabytes of test data.
    static constexpr size_t kDefaultFlushThresholdBytes = 4 * 1024 * 1024;  // 4 MiB

    // A background compaction is requested once the SSTable count
    // reaches this many. Same testability reasoning as the flush
    // threshold - tests pass a small value to force compaction quickly.
    static constexpr size_t kDefaultCompactionTriggerCount = 4;

    explicit Database(std::filesystem::path db_dir,
                       size_t flush_threshold_bytes = kDefaultFlushThresholdBytes,
                       size_t compaction_trigger_count = kDefaultCompactionTriggerCount);
    ~Database() override;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void put(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) const override;
    void remove(const std::string& key) override;
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start, const std::string& end) const override;

    // Number of on-disk SSTables currently open. Not part of the
    // KVStore interface - exposed on the concrete class so tests can
    // observe flush/compaction effects without reasoning about exact
    // byte-size arithmetic or timing.
    size_t sstable_count() const;

    // Blocks until no compaction is requested or in progress. Exists so
    // tests can deterministically wait for the background thread to
    // finish a compaction it was triggered into, rather than polling or
    // sleeping. Not needed by normal callers.
    void WaitForBackgroundCompaction() const;

private:
    // Guards every field below except the compaction-signaling ones
    // (which have their own, separate mutex - see compaction_cv_mutex_).
    //
    // Locking strategy: shared_mutex, so get()/scan() take a shared
    // (read) lock for their whole call - concurrent reads never block
    // each other - while put()/remove() (and the flush they can
    // trigger) take a unique (write) lock for theirs, serializing
    // writes against each other and against reads. This is coarser than
    // a real engine (which would typically let a WAL append proceed
    // without blocking a concurrent read of the MemTable), but it's
    // simple to reason about and correct, which matters more here.
    //
    // Compaction only takes this lock for two cheap steps: copying the
    // current sstables_ list at the start (a handful of shared_ptr
    // copies), and replacing it with the merged result at the end (one
    // assignment). The expensive part - reading every entry out of
    // several SSTables, merging them, and writing the result to a new
    // file - runs with no lock held at all, so foreground put()/get()
    // keep working while a compaction is in progress. Because the final
    // swap is a single assignment done under the exclusive lock, a
    // concurrent get()/scan() always sees either the complete pre-
    // compaction SSTable list or the complete post-compaction one -
    // never a partially-swapped state.
    mutable std::shared_mutex mutex_;

    std::filesystem::path db_dir_;
    size_t flush_threshold_bytes_;
    size_t compaction_trigger_count_;
    uint64_t next_sstable_number_ = 1;

    MemTable memtable_;
    std::unique_ptr<WriteAheadLog> wal_;
    std::vector<std::shared_ptr<SSTableReader>> sstables_;  // oldest first, newest last

    // Assumes mutex_ is already held exclusively by the caller (put()/
    // remove()) - not locked separately.
    void MaybeFlush();
    void Flush();
    void MaybeTriggerCompaction();

    // Background compaction. compaction_cv_mutex_/compaction_cv_ are a
    // deliberately separate, small mutex+condvar pair used only to wake
    // the background thread - kept apart from mutex_ (which guards the
    // actual data) so signaling never has to contend with, or be
    // confused with, the data lock.
    void CompactionThreadMain();
    void CompactOnce();

    std::thread compaction_thread_;
    mutable std::mutex compaction_cv_mutex_;
    mutable std::condition_variable compaction_cv_;
    bool compaction_requested_ = false;
    bool compaction_running_ = false;
    bool shutting_down_ = false;
};

}  // namespace tinylsm
