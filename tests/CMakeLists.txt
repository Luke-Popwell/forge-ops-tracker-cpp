add_executable(forge_ops_tracker_tests test_forge_ops_tracker.cpp)
target_link_libraries(forge_ops_tracker_tests PRIVATE forge_ops_tracker Threads::Threads)
target_compile_options(forge_ops_tracker_tests PRIVATE -g -fno-omit-frame-pointer -Wall -Wextra)

add_test(NAME forge_ops_tracker_tests COMMAND forge_ops_tracker_tests)
