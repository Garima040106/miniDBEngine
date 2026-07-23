#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tinylsm/memtable.h"

namespace tinylsm {

// See CLAUDE.md's "SSTable" section for the full format and the sparse
// index reasoning. Summary of the on-disk layout:
//
//   [ data section  ] one EncodeRecord()-framed record per entry (the
//                      exact same format WAL records use), sorted by key
//   [ index section ] sparse: every kSparseIndexInterval-th key, as
//                      (key_len uint32 LE, key bytes, offset uint64 LE)
//   [ footer        ] fixed 24 bytes: index_offset (u64 LE),
//                      index_size (u64 LE), entry_count (u32 LE,
//                      informational only), magic (u32 LE)
//
// Reusing EncodeRecord/DecodeRecord for the data section means SSTable
// entries get the same checksum-per-entry protection WAL records do,
// for free - useful for catching at-rest bit-rot, even though (unlike
// the WAL) it isn't what makes writing an SSTable crash-safe: that's the
// temp-file-then-rename pattern WriteSSTable uses instead.

// Writes `entries` (already sorted by std::map, tombstones included) out
// to a new, immutable SSTable file at `path`. Never leaves a partially-
// written file visible at `path`: writes to `path` + ".tmp" first,
// fsyncs it, then atomically renames it into place.
void WriteSSTable(const std::filesystem::path& path, const std::map<std::string, Entry>& entries);

// A read-only handle on one on-disk SSTable file. Loads the (small)
// sparse index into memory on construction; Find()/ScanRaw() use it to
// avoid scanning the whole file for one lookup.
class SSTableReader {
public:
    explicit SSTableReader(std::filesystem::path path);

    // The raw entry (value or tombstone) for `key`, or nullopt if this
    // SSTable has no entry for it at all. Mirrors MemTable::find()'s
    // tombstone-exposing contract, for the same reason: the caller
    // (Database) is the one that decides what "absent" means.
    std::optional<Entry> Find(const std::string& key) const;

    // Every entry (tombstones included) with start <= key < end, in
    // ascending key order.
    std::vector<std::pair<std::string, Entry>> ScanRaw(const std::string& start,
                                                         const std::string& end) const;

    const std::filesystem::path& path() const { return path_; }

private:
    struct IndexEntry {
        std::string key;
        size_t offset;  // offset into data_
    };

    std::filesystem::path path_;
    std::string data_;              // the file's data section, held in memory
    std::vector<IndexEntry> index_;
};

}  // namespace tinylsm
