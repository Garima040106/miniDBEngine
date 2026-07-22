#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tinylsm {

// See CLAUDE.md's "Write-ahead log" section for the full reasoning
// behind this format. Summary of the on-disk layout, one record:
//
//   offset  size       field
//   0       4          checksum   (uint32 LE - CRC32 of bytes [4, end))
//   4       1          type       (1 = PUT, 2 = DELETE)
//   5       4          key_len    (uint32 LE)
//   9       4          value_len  (uint32 LE - always 0 for DELETE)
//   13      key_len    key bytes
//   13+key_len  value_len  value bytes (absent for DELETE)
//
// This header is pure (de)serialization: no file I/O, so it's testable
// on its own and doesn't decide anything about how a log file is
// written, fsync'd, or replayed - that's the WriteAheadLog class's job.

enum class RecordType : uint8_t {
    kPut = 1,
    kDelete = 2,
};

// CRC32 (IEEE 802.3 / zlib polynomial) over an arbitrary byte range.
// Exposed on its own so it can be tested against known vectors
// independently of record encoding.
uint32_t Crc32(std::string_view data);

// Serializes one record (checksum included) ready to be appended to a
// log file as-is. For a DELETE, `value` is ignored and value_len is
// written as 0.
std::string EncodeRecord(RecordType type, std::string_view key, std::string_view value);

enum class DecodeStatus {
    // A complete, checksum-valid record was decoded.
    kOk,
    // Not enough bytes in `data` to know this is a complete record yet
    // (short header, or a header whose declared key_len/value_len needs
    // more bytes than `data` contains). This is the normal, expected
    // shape of a torn write at the tail of a log - not an error to
    // report to a user, just a signal to stop reading.
    kIncomplete,
    // A complete-length record was present but its checksum didn't
    // match (or its type byte isn't a recognized RecordType). Under a
    // single-writer, append-only log this should only ever happen at
    // the tail too (see CLAUDE.md) - treat it the same way as
    // kIncomplete: stop here, don't skip forward looking for the next
    // parseable record.
    kCorrupt,
};

struct DecodedRecord {
    RecordType type = RecordType::kPut;
    std::string key;
    std::string value;
    // Exact byte length of this record on disk (header + key + value),
    // i.e. how far to advance in `data`/the file to reach the next
    // record. Only meaningful when the return value is kOk.
    size_t bytes_consumed = 0;
};

// Attempts to decode exactly one record starting at the beginning of
// `data`. `data` may contain more bytes than one record (trailing bytes
// beyond `bytes_consumed` are simply not looked at) or fewer bytes than
// a full record (see kIncomplete above).
DecodeStatus DecodeRecord(std::string_view data, DecodedRecord* out);

}  // namespace tinylsm
