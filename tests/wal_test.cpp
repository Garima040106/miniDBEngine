#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <vector>

#include "tinylsm/wal.h"
#include "test_util.h"

using tinylsm::DecodedRecord;
using tinylsm::RecordType;
using tinylsm::WriteAheadLog;
using tinylsm::test::TempDir;

namespace {

struct AppliedRecord {
    RecordType type;
    std::string key;
    std::string value;

    bool operator==(const AppliedRecord& other) const {
        return type == other.type && key == other.key && value == other.value;
    }
};

std::vector<AppliedRecord> ReplayInto(WriteAheadLog& wal) {
    std::vector<AppliedRecord> applied;
    wal.Replay([&applied](RecordType type, const std::string& key, const std::string& value) {
        applied.push_back({type, key, value});
    });
    return applied;
}

}  // namespace

TEST_CASE("replaying a freshly-created log applies nothing", "[wal]") {
    TempDir dir;
    WriteAheadLog wal(dir.path / "wal.log");

    REQUIRE(ReplayInto(wal).empty());
}

TEST_CASE("appended records replay back in order", "[wal]") {
    TempDir dir;
    WriteAheadLog wal(dir.path / "wal.log");

    wal.Append(RecordType::kPut, "a", "1");
    wal.Append(RecordType::kPut, "b", "2");
    wal.Append(RecordType::kDelete, "a", "");
    wal.Append(RecordType::kPut, "c", "3");

    auto applied = ReplayInto(wal);

    REQUIRE(applied == std::vector<AppliedRecord>{
                            {RecordType::kPut, "a", "1"},
                            {RecordType::kPut, "b", "2"},
                            {RecordType::kDelete, "a", ""},
                            {RecordType::kPut, "c", "3"},
                        });
}

TEST_CASE("a log survives being closed and reopened - simulated restart", "[wal]") {
    TempDir dir;
    std::filesystem::path log_path = dir.path / "wal.log";

    {
        WriteAheadLog wal(log_path);
        wal.Append(RecordType::kPut, "durable", "value");
    }  // WriteAheadLog destroyed here, as if the process restarted

    WriteAheadLog reopened(log_path);
    auto applied = ReplayInto(reopened);

    REQUIRE(applied == std::vector<AppliedRecord>{{RecordType::kPut, "durable", "value"}});
}

TEST_CASE("replay stops at a torn tail and truncates it away", "[wal]") {
    TempDir dir;
    std::filesystem::path log_path = dir.path / "wal.log";

    size_t valid_size = 0;
    {
        WriteAheadLog wal(log_path);
        wal.Append(RecordType::kPut, "good", "record");
        valid_size = std::filesystem::file_size(log_path);
    }

    // Simulate a crash mid-write: append bytes that look like the start
    // of another record but are cut off, directly to the file - Append()
    // always writes a complete record, so this bypasses it on purpose.
    {
        std::ofstream raw(log_path, std::ios::binary | std::ios::app);
        raw << std::string(6, '\xAB');  // fewer than the 13-byte header
    }
    REQUIRE(std::filesystem::file_size(log_path) > valid_size);

    WriteAheadLog wal(log_path);
    auto applied = ReplayInto(wal);

    REQUIRE(applied == std::vector<AppliedRecord>{{RecordType::kPut, "good", "record"}});
    // The torn bytes must be gone, not just skipped in memory - otherwise
    // a *second* replay would hit them again and a future Append() would
    // leave them stranded mid-file.
    REQUIRE(std::filesystem::file_size(log_path) == valid_size);
}

TEST_CASE("replay stops at a corrupted-but-complete-length record", "[wal]") {
    TempDir dir;
    std::filesystem::path log_path = dir.path / "wal.log";

    size_t valid_size = 0;
    {
        WriteAheadLog wal(log_path);
        wal.Append(RecordType::kPut, "good", "record");
        wal.Append(RecordType::kPut, "also-good", "record2");
        valid_size = std::filesystem::file_size(log_path);
    }

    // Flip a byte inside the second record's value so its checksum no
    // longer matches, without changing the file's length.
    {
        std::fstream raw(log_path, std::ios::binary | std::ios::in | std::ios::out);
        raw.seekp(-1, std::ios::end);
        raw.put('\x00');
    }

    WriteAheadLog wal(log_path);
    auto applied = ReplayInto(wal);

    REQUIRE(applied == std::vector<AppliedRecord>{{RecordType::kPut, "good", "record"}});
}

TEST_CASE("Reset truncates the log and future appends start clean", "[wal]") {
    TempDir dir;
    WriteAheadLog wal(dir.path / "wal.log");

    wal.Append(RecordType::kPut, "old", "data");
    wal.Reset();

    REQUIRE(ReplayInto(wal).empty());

    wal.Append(RecordType::kPut, "new", "data");
    auto applied = ReplayInto(wal);
    REQUIRE(applied == std::vector<AppliedRecord>{{RecordType::kPut, "new", "data"}});
}
