#pragma once

// Internal (src/-only) POSIX I/O helpers shared by wal.cpp and
// sstable.cpp: both need "write every byte, retrying on EINTR" and a
// consistent way to turn errno into an exception.

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tinylsm::detail {

[[noreturn]] inline void ThrowErrno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

// Writes every byte of `data` to `fd`, retrying on EINTR and on short
// writes (write() is not obligated to write everything in one call).
// Throws std::runtime_error on any other error.
inline void WriteAll(int fd, std::string_view data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ThrowErrno("write failed");
        }
        written += static_cast<size_t>(n);
    }
}

// Reads the entire contents of `fd` (from its current position) into a
// string. Throws std::runtime_error on error.
inline std::string ReadAll(int fd) {
    std::string contents;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
        contents.append(buffer, static_cast<size_t>(n));
    }
    if (n < 0) {
        ThrowErrno("read failed");
    }
    return contents;
}

}  // namespace tinylsm::detail
