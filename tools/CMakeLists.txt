add_executable(tinylsm_crash_harness crash_recovery_harness.cpp)
target_link_libraries(tinylsm_crash_harness PRIVATE tinylsm_core)

add_executable(tinylsm_bench benchmark.cpp)
target_link_libraries(tinylsm_bench PRIVATE tinylsm_core)

# A lighter run than the default (fewer trials/records) so it's fast
# enough to sit in the normal `ctest` run alongside the unit tests.
# Use the tinylsm_crash_harness binary directly for a more thorough
# sweep (more trials, more records/trial).
add_test(NAME crash_recovery_harness COMMAND tinylsm_crash_harness 10 200)
set_tests_properties(crash_recovery_harness PROPERTIES TIMEOUT 60)
