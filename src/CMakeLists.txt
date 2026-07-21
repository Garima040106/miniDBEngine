add_library(tinylsm_core
    memtable.cpp
    database.cpp
)
target_include_directories(tinylsm_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_features(tinylsm_core PUBLIC cxx_std_17)

add_executable(tinylsm_repl repl.cpp)
target_link_libraries(tinylsm_repl PRIVATE tinylsm_core)
