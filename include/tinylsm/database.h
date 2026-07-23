#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "tinylsm/kv_store.h"
#include "tinylsm/memtable.h"
#include "tinylsm/sstable.h"
#include "tinylsm/wal.h"

namespace tinylsm {

// The engine callers actually talk to: a MemTable backed by a WAL for
// durability, with older generations preserved as immutable, on-disk
// SSTables once a MemTable is flushed. See CLAUDE.md's "Read path" /
// "Write path" sections for exactly how a request is resolved across
// these layers, and "SSTable" for the on-disk format and why flushing
// (write-temp-then-rename, then only *then* reset the WAL) is crash-safe.
class Database : public KVStore {
public:
    // A MemTable is flushed to a new SSTable once its approximate size
    // reaches this many bytes. The default is sized for the REPL/real
    // use; tests pass a much smaller value so a flush can be triggered
    // deterministically without writing megabytes of test data.
    static constexpr size_t kDefaultFlushThresholdBytes = 4 * 1024 * 1024;  // 4 MiB

    explicit Database(std::filesystem::path db_dir,
                       size_t flush_threshold_bytes = kDefaultFlushThresholdBytes);

    void put(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) const override;
    void remove(const std::string& key) override;
    std::vector<std::pair<std::string, std::string>> scan(
        const std::string& start, const std::string& end) const override;

    // Number of on-disk SSTables currently open. Not part of the
    // KVStore interface - exposed on the concrete class so tests can
    // assert a flush actually happened without reasoning about exact
    // byte-size arithmetic.
    size_t sstable_count() const { return sstables_.size(); }

private:
    void MaybeFlush();
    void Flush();

    std::filesystem::path db_dir_;
    size_t flush_threshold_bytes_;
    uint64_t next_sstable_number_ = 1;

    MemTable memtable_;
    std::unique_ptr<WriteAheadLog> wal_;
    std::vector<std::unique_ptr<SSTableReader>> sstables_;  // oldest first, newest last
};

}  // namespace tinylsm
