#include "tinylsm/wal_record.h"

#include <array>

namespace tinylsm {

namespace {

constexpr size_t kHeaderSize = 4 + 1 + 4 + 4;  // checksum + type + key_len + value_len

std::array<uint32_t, 256> MakeCrc32Table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
        table[i] = crc;
    }
    return table;
}

void PutUint32LE(std::string* out, uint32_t value) {
    out->push_back(static_cast<char>(value & 0xFF));
    out->push_back(static_cast<char>((value >> 8) & 0xFF));
    out->push_back(static_cast<char>((value >> 16) & 0xFF));
    out->push_back(static_cast<char>((value >> 24) & 0xFF));
}

uint32_t GetUint32LE(const char* bytes) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(bytes[0]))) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[3])) << 24);
}

bool IsValidRecordType(uint8_t type_byte) {
    return type_byte == static_cast<uint8_t>(RecordType::kPut) ||
           type_byte == static_cast<uint8_t>(RecordType::kDelete);
}

}  // namespace

uint32_t Crc32(std::string_view data) {
    static const std::array<uint32_t, 256> table = MakeCrc32Table();

    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char byte : data) {
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::string EncodeRecord(RecordType type, std::string_view key, std::string_view value) {
    // A DELETE never has a value on disk, regardless of what's passed in
    // `value` (callers shouldn't pass one, but this keeps the format's
    // invariant true either way).
    std::string_view value_to_write = (type == RecordType::kDelete) ? std::string_view{} : value;

    std::string body;  // everything the checksum covers: type + lengths + key + value
    body.reserve(1 + 4 + 4 + key.size() + value_to_write.size());
    body.push_back(static_cast<char>(type));
    PutUint32LE(&body, static_cast<uint32_t>(key.size()));
    PutUint32LE(&body, static_cast<uint32_t>(value_to_write.size()));
    body.append(key.data(), key.size());
    body.append(value_to_write.data(), value_to_write.size());

    std::string record;
    record.reserve(4 + body.size());
    PutUint32LE(&record, Crc32(body));
    record.append(body);
    return record;
}

DecodeStatus DecodeRecord(std::string_view data, DecodedRecord* out) {
    if (data.size() < kHeaderSize) {
        return DecodeStatus::kIncomplete;
    }

    const uint32_t stored_checksum = GetUint32LE(data.data());
    const uint8_t type_byte = static_cast<uint8_t>(data[4]);
    const uint32_t key_len = GetUint32LE(data.data() + 5);
    const uint32_t value_len = GetUint32LE(data.data() + 9);

    // key_len/value_len are each at most UINT32_MAX and size_t is 64-bit
    // on any platform this targets, so this sum can't overflow - even a
    // corrupted, garbage-valued header can't wrap size_t back around.
    const size_t total_size = kHeaderSize + static_cast<size_t>(key_len) + static_cast<size_t>(value_len);

    if (data.size() < total_size) {
        return DecodeStatus::kIncomplete;
    }

    // Everything the checksum was computed over: type + lengths + key + value.
    const std::string_view body = data.substr(4, total_size - 4);
    if (Crc32(body) != stored_checksum || !IsValidRecordType(type_byte)) {
        return DecodeStatus::kCorrupt;
    }

    out->type = static_cast<RecordType>(type_byte);
    out->key.assign(data.data() + kHeaderSize, key_len);
    out->value.assign(data.data() + kHeaderSize + key_len, value_len);
    out->bytes_consumed = total_size;
    return DecodeStatus::kOk;
}

}  // namespace tinylsm
