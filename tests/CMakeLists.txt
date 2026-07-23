include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(tinylsm_tests
    memtable_test.cpp
    database_test.cpp
    wal_record_test.cpp
    wal_test.cpp
    sstable_test.cpp
)
target_link_libraries(tinylsm_tests PRIVATE tinylsm_core Catch2::Catch2WithMain)
target_include_directories(tinylsm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(tinylsm_tests)
