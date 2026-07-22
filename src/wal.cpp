#include "tinylsm/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace tinylsm {

namespace {

[[noreturn]] void ThrowErrno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

}  // namespace

WriteAheadLog::WriteAheadLog(std::filesystem::path path) : path_(std::move(path)) {
    // O_APPEND makes every write() atomically seek-to-end-then-write at
    // the kernel level, which is what "append-only" actually requires -
    // no separate lseek() needed, here or after Reset()/truncation.
    fd_ = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd_ < 0) {
        ThrowErrno("failed to open WAL file " + path_.string());
    }
}

WriteAheadLog::~WriteAheadLog() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void WriteAheadLog::Append(RecordType type, std::string_view key, std::string_view value) {
    const std::string record = EncodeRecord(type, key, value);

    size_t written = 0;
    while (written < record.size()) {
        const ssize_t n = ::write(fd_, record.data() + written, record.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ThrowErrno("failed to write WAL record to " + path_.string());
        }
        written += static_cast<size_t>(n);
    }

    // Not durable until this returns - see CLAUDE.md for why Append()
    // must not return to its caller before this line completes.
    if (::fsync(fd_) != 0) {
        ThrowErrno("failed to fsync WAL file " + path_.string());
    }
}

void WriteAheadLog::Replay(const ApplyFn& apply) {
    const int read_fd = ::open(path_.c_str(), O_RDONLY);
    if (read_fd < 0) {
        ThrowErrno("failed to open WAL file for replay " + path_.string());
    }

    std::string contents;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = ::read(read_fd, buffer, sizeof(buffer))) > 0) {
        contents.append(buffer, static_cast<size_t>(n));
    }
    const bool read_failed = n < 0;
    ::close(read_fd);
    if (read_failed) {
        ThrowErrno("failed to read WAL file " + path_.string());
    }

    size_t valid_end = 0;
    std::string_view remaining(contents);
    while (true) {
        DecodedRecord record;
        if (DecodeRecord(remaining, &record) != DecodeStatus::kOk) {
            break;  // torn or corrupt tail - stop, don't skip forward (see CLAUDE.md)
        }
        apply(record.type, record.key, record.value);
        valid_end += record.bytes_consumed;
        remaining = remaining.substr(record.bytes_consumed);
    }

    if (valid_end < contents.size()) {
        if (::ftruncate(fd_, static_cast<off_t>(valid_end)) != 0) {
            ThrowErrno("failed to truncate torn tail from WAL file " + path_.string());
        }
    }
}

void WriteAheadLog::Reset() {
    if (::ftruncate(fd_, 0) != 0) {
        ThrowErrno("failed to reset WAL file " + path_.string());
    }
}

}  // namespace tinylsm
