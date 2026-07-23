#pragma once

// Internal (src/-only, not part of the public include/tinylsm/ API)
// little-endian integer encode/decode helpers, shared by wal_record.cpp
// and sstable.cpp - both formats frame records/index entries the same
// way, down to this level.

#include <cstdint>
#include <string>

namespace tinylsm::detail {

inline void PutUint32LE(std::string* out, uint32_t value) {
    out->push_back(static_cast<char>(value & 0xFF));
    out->push_back(static_cast<char>((value >> 8) & 0xFF));
    out->push_back(static_cast<char>((value >> 16) & 0xFF));
    out->push_back(static_cast<char>((value >> 24) & 0xFF));
}

inline uint32_t GetUint32LE(const char* bytes) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(bytes[0]))) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[3])) << 24);
}

inline void PutUint64LE(std::string* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out->push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

inline uint64_t GetUint64LE(const char* bytes) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
    }
    return value;
}

}  // namespace tinylsm::detail
