#include <catch2/catch_test_macros.hpp>
#include <string>

#include "tinylsm/wal_record.h"

using tinylsm::Crc32;
using tinylsm::DecodedRecord;
using tinylsm::DecodeRecord;
using tinylsm::DecodeStatus;
using tinylsm::EncodeRecord;
using tinylsm::RecordType;

// Standard CRC32 (IEEE 802.3 / zlib polynomial) test vectors - if these
// fail, the table or the reflection direction is wrong.
TEST_CASE("Crc32 matches the standard test vectors", "[wal_record]") {
    REQUIRE(Crc32("") == 0x00000000u);
    REQUIRE(Crc32("123456789") == 0xCBF43926u);
}

TEST_CASE("a PUT record round-trips through encode and decode", "[wal_record]") {
    std::string encoded = EncodeRecord(RecordType::kPut, "key", "value");

    DecodedRecord decoded;
    REQUIRE(DecodeRecord(encoded, &decoded) == DecodeStatus::kOk);
    REQUIRE(decoded.type == RecordType::kPut);
    REQUIRE(decoded.key == "key");
    REQUIRE(decoded.value == "value");
    REQUIRE(decoded.bytes_consumed == encoded.size());
}

TEST_CASE("a DELETE record round-trips with an empty value on the wire", "[wal_record]") {
    std::string encoded = EncodeRecord(RecordType::kDelete, "key", /*value=*/"ignored-if-passed");

    DecodedRecord decoded;
    REQUIRE(DecodeRecord(encoded, &decoded) == DecodeStatus::kOk);
    REQUIRE(decoded.type == RecordType::kDelete);
    REQUIRE(decoded.key == "key");
    REQUIRE(decoded.value.empty());
    // The value was never written on the wire at all, not just decoded
    // as empty - the encoded record shouldn't have grown to fit it.
    REQUIRE(encoded.size() == 13 + std::string("key").size());
}

TEST_CASE("empty key and value are valid and round-trip", "[wal_record]") {
    std::string encoded = EncodeRecord(RecordType::kPut, "", "");

    DecodedRecord decoded;
    REQUIRE(DecodeRecord(encoded, &decoded) == DecodeStatus::kOk);
    REQUIRE(decoded.key.empty());
    REQUIRE(decoded.value.empty());
}

TEST_CASE("decoding an empty buffer reports incomplete, not corrupt", "[wal_record]") {
    DecodedRecord decoded;
    REQUIRE(DecodeRecord("", &decoded) == DecodeStatus::kIncomplete);
}

TEST_CASE("a buffer shorter than the fixed header reports incomplete", "[wal_record]") {
    std::string encoded = EncodeRecord(RecordType::kPut, "key", "value");
    std::string torn_header = encoded.substr(0, 5);  // less than the 13-byte header

    DecodedRecord decoded;
    REQUIRE(DecodeRecord(torn_header, &decoded) == DecodeStatus::kIncomplete);
}

TEST_CASE("a buffer with a full header but a torn key reports incomplete", "[wal_record]") {
    std::string encoded = EncodeRecord(RecordType::kPut, "a-longer-key", "value");
    std::string torn_key = encoded.substr(0, 13 + 3);  // header + first 3 bytes of the key

    DecodedRecord decoded;
    REQUIRE(DecodeRecord(torn_key, &decoded) == DecodeStatus::kIncomplete);
}

TEST_CASE("a buffer with a full header and key but a torn value reports incomplete", "[wal_record]") {
    std::string encoded = EncodeRecord(RecordType::kPut, "key", "a-longer-value");
    std::string torn_value = encoded.substr(0, encoded.size() - 3);  // missing the last 3 value bytes

    DecodedRecord decoded;
    REQUIRE(DecodeRecord(torn_value, &decoded) == DecodeStatus::kIncomplete);
}

TEST_CASE("a single flipped byte in an otherwise complete record is reported as corrupt",
          "[wal_record]") {
    std::string encoded = EncodeRecord(RecordType::kPut, "key", "value");
    encoded[encoded.size() - 1] ^= 0xFF;  // flip a bit inside the value

    DecodedRecord decoded;
    REQUIRE(DecodeRecord(encoded, &decoded) == DecodeStatus::kCorrupt);
}

TEST_CASE("a checksum-valid record with an unrecognized type byte is reported as corrupt",
          "[wal_record]") {
    // Building this by hand rather than via EncodeRecord, since
    // EncodeRecord can only ever produce valid type bytes - this
    // isolates "checksum matches but the type is bogus" specifically.
    std::string body;
    body.push_back(static_cast<char>(99));  // not kPut(1) or kDelete(2)
    body.append(4, '\0');                   // key_len = 0
    body.append(4, '\0');                   // value_len = 0
    uint32_t checksum = Crc32(body);

    std::string record;
    record.push_back(static_cast<char>(checksum & 0xFF));
    record.push_back(static_cast<char>((checksum >> 8) & 0xFF));
    record.push_back(static_cast<char>((checksum >> 16) & 0xFF));
    record.push_back(static_cast<char>((checksum >> 24) & 0xFF));
    record.append(body);

    DecodedRecord decoded;
    REQUIRE(DecodeRecord(record, &decoded) == DecodeStatus::kCorrupt);
}

TEST_CASE("bytes_consumed lands exactly on the start of the next concatenated record",
          "[wal_record]") {
    std::string first = EncodeRecord(RecordType::kPut, "a", "1");
    std::string second = EncodeRecord(RecordType::kDelete, "b", "");
    std::string buffer = first + second;

    DecodedRecord decoded_first;
    REQUIRE(DecodeRecord(buffer, &decoded_first) == DecodeStatus::kOk);
    REQUIRE(decoded_first.bytes_consumed == first.size());

    std::string_view remaining(buffer.data() + decoded_first.bytes_consumed,
                                buffer.size() - decoded_first.bytes_consumed);
    DecodedRecord decoded_second;
    REQUIRE(DecodeRecord(remaining, &decoded_second) == DecodeStatus::kOk);
    REQUIRE(decoded_second.type == RecordType::kDelete);
    REQUIRE(decoded_second.key == "b");
}

TEST_CASE("DecodeRecord ignores trailing bytes belonging to a later record", "[wal_record]") {
    std::string first = EncodeRecord(RecordType::kPut, "a", "1");
    std::string second = EncodeRecord(RecordType::kPut, "b", "2");
    std::string buffer = first + second;

    DecodedRecord decoded;
    REQUIRE(DecodeRecord(buffer, &decoded) == DecodeStatus::kOk);
    REQUIRE(decoded.key == "a");
    REQUIRE(decoded.bytes_consumed == first.size());
}
