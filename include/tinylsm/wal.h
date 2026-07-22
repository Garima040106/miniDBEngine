#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "tinylsm/wal_record.h"

namespace tinylsm {

// The file-handling layer on top of wal_record.h's pure (de)serialization:
// opening the log file, write()+fsync() on append, and read+validate+
// truncate on replay. See CLAUDE.md's "Write-ahead log" section for the
// full design reasoning behind every decision here.
class WriteAheadLog {
public:
    // Opens (creating if necessary) the log file at `path` for appending.
    // Throws std::runtime_error if the file can't be opened.
    explicit WriteAheadLog(std::filesystem::path path);
    ~WriteAheadLog();

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    // Appends one record and fsyncs before returning - this call does
    // not return until the write is durable. Throws std::runtime_error
    // on any I/O failure (this is treated as an invariant violation, not
    // an expected outcome - see CLAUDE.md's "Conventions").
    void Append(RecordType type, std::string_view key, std::string_view value);

    // Replays every valid record from the start of the file, in order,
    // invoking `apply` for each one. Stops at the first torn/corrupt
    // record rather than treating it as an error: whatever was validly
    // logged (and therefore was, or could have been, acknowledged) gets
    // applied, and nothing past that point is trusted.
    //
    // After replay, the log file is truncated to the offset right after
    // the last valid record, discarding any torn tail bytes - otherwise
    // they'd sit forever in the middle of the file (new appends go
    // *after* them, not over them) and the next replay would hit that
    // same garbage first and stop early, even though everything
    // appended after it this run is perfectly valid.
    using ApplyFn = std::function<void(RecordType, const std::string& key, const std::string& value)>;
    void Replay(const ApplyFn& apply);

    // Truncates the log to empty. Called after a successful flush to an
    // SSTable, once the memtable generation this log backs no longer
    // needs to be replayed.
    void Reset();

private:
    std::filesystem::path path_;
    int fd_ = -1;
};

}  // namespace tinylsm
