cmake_minimum_required(VERSION 3.16)
project(forge_ops_tracker CXX)

set(CMAKE_CXX_STANDARD 17) # the headers already use std::optional throughout (Configuration, event_builder's own overloads): C++17 is the real floor, not a rounder-sounding default
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED) # DeliveryQueue's background std::thread

# nlohmann/json is a real, header-only third-party dependency this client already reaches for
# throughout its own headers (event_builder.hpp, client.hpp, delivery_queue.hpp, reporter.hpp,
# pii_scrubber.hpp all include <nlohmann/json.hpp> directly): unlike sdks/c, sdks/rust, and
# sdks/go, this SDK's JSON encoding was never hand-rolled to begin with. That's an existing design
# decision baked into the source this round inherited, not something introduced here; see the
# README's own "Dependencies" section for the full reasoning on why it was kept rather than ripped
# out this late. find_package first (nlohmann_json ships a proper CMake config almost everywhere
# it's packaged, including Homebrew), falling back to FetchContent only if it's genuinely not
# already installed, so a machine that already has it (like this one) never has to build it twice.
find_package(nlohmann_json 3 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(forge_ops_tracker
    src/configuration.cpp
    src/pii_scrubber.cpp
    src/event_builder.cpp
    src/delivery_queue.cpp
    src/client.cpp
    src/reporter.cpp
    src/performance_flusher.cpp
    src/span_buffer.cpp
    src/metric_buffer.cpp
    src/span_queue.cpp
    src/forge_ops_tracker.cpp
)
target_include_directories(forge_ops_tracker PUBLIC include)
# Both PUBLIC: nlohmann::json and CURL types don't leak into consumer-visible signatures for CURL,
# but nlohmann::json does (every public header above takes/returns one), so a consumer linking
# this library needs its include path too, not just this library's own translation units.
target_link_libraries(forge_ops_tracker PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
target_link_libraries(forge_ops_tracker PRIVATE Threads::Threads)

# Real (not stub) backtrace symbol resolution needs frame pointers and debug info: without
# -fno-omit-frame-pointer, backtrace() can silently return far fewer/wrong frames under
# optimization, which would make event_builder's own tests flaky depending on build type. Same
# reasoning as sdks/c's own CMakeLists.txt.
target_compile_options(forge_ops_tracker PRIVATE -g -fno-omit-frame-pointer -Wall -Wextra)

# No extra link flag needed for _NSGetExecutablePath on Apple platforms (part of libSystem):
# event_builder.cpp's own #if defined(__APPLE__) branch is what actually gates that code, not
# anything here.

enable_testing()
add_subdirectory(tests)
