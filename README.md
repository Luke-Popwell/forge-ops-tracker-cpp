# forge_ops_tracker (C++)

C++ error reporting client for a private, self-hosted [ForgeOps](../../) tracker instance.
Requires a POSIX platform (macOS, Linux) -- see "Platform" below. Targets C++17.

Shaped like this repo's own Go and Rust clients (a live delivery queue: Configuration,
EventBuilder, DeliveryQueue, Reporter, Client), not the Objective-C/Swift/C clients' disk-backed
crash-reporter model. That's a real, deliberate difference worth calling out up front: C++ *does*
have exceptions (unlike plain C), so "catch it, build a payload, hand it to a background thread"
is both possible and the shape this SDK's own source already took before this round's build system
and tests were added -- there was no crash-reporter/live-queue decision left to make here, only to
confirm by reading the actual `Reporter`/`DeliveryQueue` source, which is exactly what this round
did before writing anything else.

## Installation

There's no package registry for C++ the way npm/PyPI/etc. work for other languages -- CMake's own
`FetchContent`, pointed at a real tagged release, is the closest equivalent:

```cmake
# your own CMakeLists.txt
include(FetchContent)
FetchContent_Declare(
  forge_ops_tracker
  GIT_REPOSITORY https://github.com/Luke-Popwell/forge-ops-tracker-cpp.git
  GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(forge_ops_tracker)
target_link_libraries(your_app PRIVATE forge_ops_tracker)
```

That's a mirror, kept in sync automatically from `sdks/cpp` in the main `forge_ops` repo (which is
private, so isn't itself something `FetchContent` could ever pull directly) -- develop against
that repo, not this one. To build and run this SDK's own tests directly instead:

```bash
cd sdks/cpp
cmake -B build
cmake --build build
./build/tests/forge_ops_tracker_tests
# or: cd build && ctest --output-on-failure
```

### Dependencies

Two real dependencies, not one:

- **libcurl**, for HTTP delivery. The same reasoning as this repo's own C client: there's no HTTP
  client anywhere in the C++ standard library, and hand-rolling raw HTTP/1.1-over-TLS from a bare
  socket is a security-sensitive undertaking no reasonable client should attempt from scratch.
- **[nlohmann/json](https://github.com/nlohmann/json)**, for the event payload's JSON encoding.

That second one is worth being honest about rather than glossing over: this repo's general
convention for a language with no JSON type of its own (see `sdks/c`, `sdks/rust`, `sdks/go`) is to
hand-roll a small encoder instead of adding a dependency for it. This client doesn't follow that
convention -- `<nlohmann/json.hpp>` is included directly in five of its seven public headers
(`event_builder.hpp`, `client.hpp`, `delivery_queue.hpp`, `reporter.hpp`, `pii_scrubber.hpp`), and
`nlohmann::json` is the type every one of those headers' own public signatures is built around, not
an implementation detail hidden behind a `.cpp` file. That was already true of the source this
round of work started from, not a choice made while building the CMake/test/README layer on top of
it -- ripping it out and hand-rolling a JSON encoder instead would mean rewriting the public API of
every header in this library, which is a materially different (and much larger) task than "add a
build system and tests for what's already here." nlohmann/json is header-only, extremely widely
used, and MIT-licensed, which makes it a reasonable dependency to have kept rather than a red flag
-- but it is a second real dependency beyond libcurl, and this SDK's own `CMakeLists.txt` and this
README should say so plainly instead of the "one dependency, hand-rolled JSON" story that would
otherwise match every other SDK in this repo's own pattern. `CMakeLists.txt` looks for an
already-installed `nlohmann_json` first (via `find_package`, which it ships a working CMake config
for almost everywhere it's packaged, including Homebrew) and only falls back to fetching it itself
via `FetchContent` if that's not found.

### Platform

POSIX only (confirmed by actually building and running the full test suite on macOS/AppleClang;
Linux/glibc is expected to behave identically -- `<execinfo.h>`, `<regex>`, `std::thread`, and
libcurl are all standard there too -- but that expectation is not independently verified the way
the macOS build is, since no Linux machine was available in this environment; the one place this
genuinely could differ is `backtrace_symbols()`'s exact per-frame text format, which `event_builder.cpp`'s
own frame-parsing regex assumes is the same de facto convention on both platforms). Not Windows: no
`<execinfo.h>`, no `<mach-o/dyld.h>`, no POSIX signal handling.

## Configuration

Set a DSN (from a project's settings page in ForgeOps) explicitly -- unlike this repo's Ruby,
Python, and C clients, there's no `FORGE_OPS_DSN` environment variable read automatically anywhere
in this source; confirmed directly (`grep`) rather than assumed, since every other SDK in this repo
does read one. If your deployment wants that behavior, read `std::getenv("FORGE_OPS_DSN")` yourself
before calling `init`:

```cpp
#include <forge_ops_tracker/forge_ops_tracker.hpp>

forge_ops_tracker::init([](forge_ops_tracker::Configuration& config) {
    config.dsn = "https://<api_key>@your-forgeops-host/api/v1/events";
    config.environment = "production";
    config.release = "1.4.2";
    config.app_root = "/opt/myapp"; // used for in_app backtrace classification, see below
});
```

`init()` returns a mutable `Configuration&` you can also hold onto and mutate later. Call it once
at startup, before installing the terminate handler or reporting anything.

## Usage

**Report an exception you've already caught** -- the common case, and the one every convenience
overload below is built around:

```cpp
try {
    do_something_risky();
} catch (const std::exception& e) {
    forge_ops_tracker::capture_exception(e, {{"order_id", "42"}}); // context is any nlohmann::json object
}
```

A non-`std::exception` throw (legal in C++ -- any type at all can be thrown) is still reportable,
just with no class name or message available, via `std::exception_ptr`:

```cpp
try {
    do_something_risky();
} catch (...) {
    forge_ops_tracker::capture_exception(std::current_exception());
}
```

**A genuinely uncaught exception needs no further wiring at all**, once
`forge_ops_tracker::install_terminate_handler()` has run at startup. `std::terminate()` is where
C++ ends up when an exception propagates past every handler -- this installs a handler there that
reports whatever's in flight (via `std::current_exception()`) and then chains to whatever handler
was previously installed (the default one, unless something else in your process also called
`std::set_terminate`), so the process still terminates exactly as it would have without this
client, just with a report sent first.

You don't have to use the package-level `init`/`capture_exception` singleton at all -- every piece
(`Configuration`, `EventBuilder`, `DeliveryQueue`, `Client`, `Reporter`) is a real, independently
constructible class, the same shape this repo's Go and Rust clients expose alongside their own
package-level convenience API, for anyone who wants multiple independently configured trackers in
one process instead of one shared global.

## How delivery works: a bounded queue, drained by a background thread

Every report is pushed onto a `DeliveryQueue` (bounded by `Configuration::queue_size`, default
1000) and handed off to a single background `std::thread` that drains it one item at a time over
HTTP via `Client` (libcurl). `push()` itself never blocks the calling thread waiting on a network
call -- it only briefly holds a mutex to enqueue -- so reporting an error from a request-handling
thread can't stall that thread on a slow or unreachable tracker. If the queue is already at
capacity when `push()` is called (the background worker is behind, typically because delivery is
slow or the tracker is unreachable), the new event is dropped and logged rather than blocking or
growing unbounded; `Configuration::logger`, if set, is called with a message describing the drop.

The worker thread is started lazily, on the first `push()`, not at construction -- not for a
fork-safety reason the way the Ruby/Python/Node clients' own lazy start is (a C++ binary has no
interpreter-level "module load" moment a prefork server could fork after), but simply so a
`Reporter` that's constructed but never actually used to report anything never spins up a thread it
doesn't need.

Destroying a `DeliveryQueue` (its destructor) signals the worker to stop and joins it -- but the
worker's own loop keeps draining whatever's still queued before actually returning, rather than
abandoning it mid-queue. That means destruction can block for as long as delivering the remaining
items takes (up to `Configuration::timeout_seconds` each, if the tracker is slow or unreachable) --
a real, worth-knowing behavior, not a bug: it trades "might block briefly at shutdown" for "don't
silently drop everything still in flight when the process is tearing down normally." Confirmed
directly by a real test (`delivery_queue_destructor_drains_pending_items_before_returning`), not
just read off the source.

## Backtrace frames: image + symbol, never file/line -- and captured where you catch, not where you throw

Same situation as this repo's own Objective-C/Swift/C clients: `backtrace()`/`backtrace_symbols()`
(`<execinfo.h>`) give a binary image name (closest available analog to "file") and a resolved,
demangled symbol (closest analog to "method"); `line` is always JSON `null` -- a compiled,
optimized C++ binary has no source file/line information left in it at runtime the way an
interpreted language's own stack trace carries.

The backtrace is captured at the moment `build()` (or `capture_exception`/`report`) is *called*,
not at the moment the exception was originally thrown -- C++ exceptions carry no stack trace of
their own, unlike Ruby's backtrace or Python's traceback attached to the exception object itself.
Confirmed directly on this machine: by the time a `catch` block runs, the stack has typically
already unwound back up through most of the intervening call frames, so a real backtrace captured
from inside a `catch` block is usually much shorter than the call stack that actually led to the
`throw` -- call `capture_exception`/`report` as close to the `catch` site as possible for the most
useful trace, the same honest caveat this repo's PHP client documents for its own capture-timing
quirk.

`in_app` compares a frame's image name against the running process's own executable name
(`_NSGetExecutablePath` on macOS, via `<mach-o/dyld.h>`) -- true only for frames inside your own
binary, `false` for anything from a shared library. Since this test suite links the library
statically into its own test binary, every frame is technically "your own binary" in that build --
a real caveat for any consumer who does the same (link statically rather than against a `.so`/
`.dylib`), documented here rather than silently left as a surprise: `in_app` degrades to "always
true" when the library and the app share one binary image, the same limitation `is_in_app`'s own
comment in `event_builder.cpp` already flags.

## PII scrubbing

Same behavior as this repo's own Ruby original, using `std::regex`'s default ECMAScript grammar,
which -- unlike the C client's POSIX `<regex.h>` adaptation -- accepts all 8 of the original
patterns (including `\b` word boundaries) unmodified; verified directly against real matching
input for each pattern (see the test file's own `pii_*` tests), not assumed to translate cleanly
just because the syntax looks the same.

The message, backtrace, and any context you attach are scanned for likely personal data -- email
addresses, formatted SSNs/credit cards, known API key/token formats (AWS, Stripe, GitHub, JWT,
bearer tokens), and anything under a suspiciously-named key (`password`, `api_key`, `ssn`, and
similar, matched case- and punctuation-insensitively) -- and redacted before the payload ever
leaves this process. ForgeOps itself scrubs again on arrival regardless, so this is a second,
earlier layer, not the only one. Deliberately does *not* support `Project#additional_sensitive_keys`
(server-side only, by design -- see `pii_scrubber.hpp`'s own header comment).

To disable it: `config.scrub_pii = false;`

## Running the tests

```bash
cd sdks/cpp
cmake -B build
cmake --build build
./build/tests/forge_ops_tracker_tests
# or: cd build && ctest --output-on-failure
```

A hand-rolled test runner (`TEST`/`RUN`/`ASSERT_TRUE` macros), not an external framework like
Catch2 or GoogleTest -- a real decision, not an oversight, and the reasoning is worth stating
explicitly rather than leaving unstated: this client's own toolchain and scope are closest in
spirit to `sdks/c` (same libcurl/`<execinfo.h>`/POSIX foundations, same single-platform-shaped
client rather than a portable-everywhere one), and `sdks/c`'s own test file already made and
documented this same call for the same reasons. Reaching for a vendored test framework purely for
the tests themselves, on top of the two real runtime dependencies this library already has
(libcurl, nlohmann_json -- see "Dependencies" above), would add a third dependency for no
capability the ~80 lines of macros at the top of the test file don't already provide. C++
exceptions genuinely make the hand-rolled approach less painful here than in C: `RUN()` wraps each
test in a `try`/`catch` so an assertion failure (or any unexpected exception) fails just that one
test and moves on, instead of every `ASSERT` needing its own manual "set a flag and `return`" dance
the way the C version's macros do.

The test suite covers every component with real assertions, including real local HTTP delivery
(a small `TestServer` helper spins up a background-thread HTTP server on `127.0.0.1`, the same
technique -- and the same lesson learned about reading the *full* request before responding, to
avoid racing curl's own write -- as `sdks/c`'s own test server) and a real bounded-queue drop test
against a deliberately non-responding "black hole" listener. All 34 tests pass under both a plain
build and a build with AddressSanitizer + UndefinedBehaviorSanitizer
(`-fsanitize=address,undefined`) -- zero sanitizer findings.

The terminate handler's own body isn't exercised by this test suite beyond confirming installation
is idempotent -- deliberately: actually letting an exception escape uncaught to trigger it for real
would terminate the test process itself, the same reason `sdks/c`'s fatal-signal handler gets the
same treatment in its own test file.

### A real bug this test suite caught

Both convenience overloads that accept an already-caught exception by reference --
`EventBuilder::build(const std::exception&, ...)` and `Reporter::report(const std::exception&,
...)` -- originally rebuilt an `exception_ptr` via `std::make_exception_ptr(exception)`. That looks
correct but silently slices: template argument deduction for `make_exception_ptr` picks its
template parameter from `exception`'s *declared* parameter type (`const std::exception&`), not its
real dynamic type, so a caught custom exception (say, a `std::runtime_error` subclass with its own
message) came back through the rebuilt `exception_ptr` as a bare `std::exception` -- both its real
class name and its own `what()` message replaced with `std::exception`'s generic boilerplate text.
Verified directly with a standalone repro before touching the fix (a derived exception's message
came back as the literal string `"std::exception"`, not the message it was actually constructed
with), then caught again by this SDK's own `event_builder_basic_fields` and
`event_builder_scrubs_message_and_context_by_default` tests, which failed against the *unfixed*
source with exactly that symptom before the fix below was applied.

The fix: both functions now prefer `std::current_exception()`, which has no such problem -- it
reflects whatever's genuinely in flight, and calling it from inside the same `catch` block that
caught the exception (even through another function call, like `capture_exception` calling into
`Reporter::report`) still counts as "being handled" per the standard. Both fall back to the
original `make_exception_ptr(exception)` behavior only when nothing is actually in flight (a caller
building/reporting a `std::exception` object it never threw) -- that edge case genuinely can't
recover a derived type through a plain base-class reference; no implementation can work around
that, it's a hard C++ limitation rather than a bug in this one. See the comments on both fixed
functions (`event_builder.cpp`, `reporter.cpp`) for the full reasoning inline.

This means anyone who had already built against the *original* unfixed source and relied on
`capture_exception(e)`/`report(e)` from inside a real `catch (const std::exception& e)` block was
silently getting `"std::exception"` as both the exception class and the message for every report
sent that way -- worth knowing if this SDK is ever pulled from a point before this fix.
