add_executable(forge_ops_tracker_tests test_forge_ops_tracker.cpp)
target_link_libraries(forge_ops_tracker_tests PRIVATE forge_ops_tracker Threads::Threads)
target_compile_options(forge_ops_tracker_tests PRIVATE -g -fno-omit-frame-pointer -Wall -Wextra)

add_test(NAME forge_ops_tracker_tests COMMAND forge_ops_tracker_tests)

# The strict privacy vectors are a copy of sdks/strict_privacy_vectors.json, kept byte identical, so
# the tests read them from the source tree wherever the build directory is.
target_compile_definitions(forge_ops_tracker_tests PRIVATE FORGE_OPS_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
