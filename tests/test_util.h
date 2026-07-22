#pragma once

#include <filesystem>
#include <random>
#include <string>

namespace tinylsm::test {

// A fresh, empty directory that removes itself (recursively) when it
// goes out of scope - used by any test that needs real files on disk
// (WAL, SSTable, Database).
struct TempDir {
    std::filesystem::path path;

    TempDir()
        : path(std::filesystem::temp_directory_path() /
                ("tinylsm_test_" + std::to_string(std::random_device{}()))) {
        std::filesystem::create_directories(path);
    }

    ~TempDir() { std::filesystem::remove_all(path); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

}  // namespace tinylsm::test
