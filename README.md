# forge_ops_tracker (C++)

C++ error reporting client for [ForgeOps](https://getforgeops.net).
Requires a POSIX platform (macOS, Linux): see "Platform" below. Targets C++17.

Built around a live delivery queue (Configuration, EventBuilder, DeliveryQueue, Reporter, Client)
rather than a disk-backed crash store. C++ has real exceptions, so "catch it, build a payload, hand
it to a background thread" is both possible and the shape this SDK takes: an exception is caught at
a normal call site, not recovered from a terminating process, so there's no need to persist it to
disk before it can be delivered.

## Installation

There's no package registry for C++ the way npm/PyPI/etc. work for other languages: CMake's own
`FetchContent`, pointed at a real tagged release, is the closest equivalent:

```cmake
# your own CMakeLists.txt
include(FetchContent)
FetchContent_Declare(
  forge_ops_tracker
  GIT_REPOSITORY https://github.com/Luke-Popwell/forge-ops-tracker-cpp.git
  GIT_TAG v0.6.0
)
FetchContent_MakeAvailable(forge_ops_tracker)
target_link_libraries(your_app PRIVATE forge_ops_tracker)
```

That's a mirror, kept in sync automatically from `sdks/cpp` in the main `forge_ops` repo (which is
private, so isn't itself something `FetchContent` could ever pull directly): develop against
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

- **libcurl**, for HTTP delivery: there's no HTTP client anywhere in the C++ standard library, and
  hand-rolling raw HTTP/1.1-over-TLS from a bare socket is a security-sensitive undertaking no
  reasonable client should attempt from scratch.
- **[nlohmann/json](https://github.com/nlohmann/json)**, for the event payload's JSON encoding.

That second one is worth being honest about rather than glossing over: C++ has no JSON type of its
own, and a small hand-rolled encoder is a viable option for that gap, but that isn't the path this
client takes. `<nlohmann/json.hpp>` is included directly in five of its seven public headers
(`event_builder.hpp`, `client.hpp`, `delivery_queue.hpp`, `reporter.hpp`, `pii_scrubber.hpp`), and
`nlohmann::json` is the type every one of those headers' own public signatures is built around, not
an implementation detail hidden behind a `.cpp` file. Ripping it out and hand-rolling a JSON
encoder instead would mean rewriting the public API of every header in this library. nlohmann/json
is header-only, extremely widely used, and MIT-licensed, which makes it a reasonable dependency to
keep rather than a red flag, but it is a second real dependency beyond libcurl, and this SDK's own
`CMakeLists.txt` and this README say so plainly. `CMakeLists.txt` looks for an already-installed
`nlohmann_json` first (via `find_package`, which it ships a working CMake config for almost
everywhere it's packaged, including Homebrew) and only falls back to fetching it itself via
`FetchContent` if that's not found.

### Platform

POSIX only (confirmed by actually building and running the full test suite on macOS/AppleClang;
Linux/glibc is expected to behave identically: `<execinfo.h>`, `<regex>`, `std::thread`, and
libcurl are all standard there too, but that expectation is not independently verified the way
the macOS build is, since no Linux machine was available in this environment; the one place this
genuinely could differ is `backtrace_symbols()`'s exact per-frame text format, which `event_builder.cpp`'s
own frame-parsing regex assumes is the same de facto convention on both platforms). Not Windows: no
`<execinfo.h>`, no `<mach-o/dyld.h>`, no POSIX signal handling.

## Configuration

Set a DSN (from a project's settings page in ForgeOps) explicitly: there's no `FORGE_OPS_DSN`
environment variable read automatically anywhere in this source; confirmed directly (`grep`) rather
than assumed. If your deployment wants that behavior, read `std::getenv("FORGE_OPS_DSN")` yourself
before calling `init`:

```cpp
#include <forge_ops_tracker/forge_ops_tracker.hpp>

forge_ops_tracker::init([](forge_ops_tracker::Configuration& config) {
    config.dsn = "https://<api_key>@getforgeops.net/api/v1/events";
    config.environment = "production";
    config.release = "1.4.2";
    config.app_root = "/opt/myapp"; // used for in_app backtrace classification, see below
});
```

`init()` returns a mutable `Configuration&` you can also hold onto and mutate later. Call it once
at startup, before installing the terminate handler or reporting anything.

## Usage

**Report an exception you've already caught**: the common case, and the one every convenience
overload below is built around:

```cpp
try {
    do_something_risky();
} catch (const std::exception& e) {
    forge_ops_tracker::capture_exception(e, {{"order_id", "42"}}); // context is any nlohmann::json object
}
```

A non-`std::exception` throw (legal in C++: any type at all can be thrown) is still reportable,
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
C++ ends up when an exception propagates past every handler: this installs a handler there that
reports whatever's in flight (via `std::current_exception()`) and then chains to whatever handler
was previously installed (the default one, unless something else in your process also called
`std::set_terminate`), so the process still terminates exactly as it would have without this
client, just with a report sent first.

You don't have to use the package-level `init`/`capture_exception` singleton at all: every piece
(`Configuration`, `EventBuilder`, `DeliveryQueue`, `Client`, `Reporter`) is a real, independently
constructible class, for anyone who wants multiple independently configured trackers in one process
instead of one shared global.

## Identifying users

```cpp
forge_ops_tracker::capture_exception(e, {{"order_id", "42"}}, {{"id", user.id}, {"email", user.email}});
```

Or `set_user` to attach it to every subsequently reported exception on this thread (an explicit
`capture_exception` call with no `user` argument, or whatever the installed terminate handler
reports) until changed or cleared, rather than passing it to every call by hand, e.g. right after
authenticating a request:

```cpp
forge_ops_tracker::set_user({{"id", user.id}, {"email", user.email}});
// once the request is done, or on sign-out:
forge_ops_tracker::set_user({});
```

There's no way to automatically detect "the current user" the way a server-side web framework with
its own session/auth middleware can, so this is always manual. `set_user` is a plain `thread_local`,
not a process-wide global: the right choice for a server handling more than one request at a time,
each on its own thread, the same reasoning `gems/forge_ops_tracker` documents for its own
`Thread.current` use; a single-threaded program just has the one thread's worth of state, so this
still behaves like a plain global there. `user` can be any `nlohmann::json` object; there's no fixed
key list enforced, follow whatever shape your own app uses. Shows up on an issue's own detail page,
and as its own affected-users count alongside the regular event count.

## Breadcrumbs

A small, bounded trail of recent events attached to whatever gets reported next, so an issue's
detail page can show what led up to it, not just the moment it happened:

```cpp
forge_ops_tracker::add_breadcrumb("charging card", "payment", "info", {{"order_id", order.id}});
```

`category` and `level` default to `"custom"`/`"info"`; `data` is any small `nlohmann::json` object.
Only the 30 most recent (`Configuration::max_breadcrumbs`) are kept, oldest dropped first; turn it
off with `Configuration::track_breadcrumbs = false`. `message` and `data` are PII-scrubbed like the
rest of the payload; `category`, `level`, and `timestamp` are structured values and never touched.
Omitted from the payload entirely when the trail is empty.

There's no web framework integration in this client to record one from automatically, so every
breadcrumb here is one you add by hand. Like `set_user`, the trail is a plain `thread_local`: right
for a server that handles one request per thread, but a thread that serves several units of work in
a row (a thread pool worker, say) must call `forge_ops_tracker::clear_breadcrumbs()` itself at the
start of each one, or the previous one's trail carries over. This client has no middleware to do
that for you.

## Performance monitoring

Times whatever you wrap and reports one small aggregate per transaction (how many times it ran,
total and maximum duration) every `Configuration::performance_flush_interval` (60s by default), for
the Performance page's per-transaction table. Not one network call per timed call.

Each aggregate also carries a small latency histogram (a count per fixed latency bucket: 50, 100,
250, 500, 1000, 2500, 5000 and 10000ms, plus an overflow bucket), so ForgeOps can show an
approximate p50/p95/p99 per transaction, not just an average. Percentiles are accurate to the width
of whichever bucket a duration falls into; the SDK never stores the individual durations.

```cpp
// RAII: records how long the scope lived, even if it throws.
{
    forge_ops_tracker::ScopedTransaction timing("GET /users/:id");
    handle_request(request);
}

// Or wrap a callable, and keep its return value:
auto user = forge_ops_tracker::time_transaction("load-user", [&] { return load_user(id); });

// Or record a duration you measured yourself, in milliseconds:
forge_ops_tracker::record_performance("nightly-export", elapsed_ms);
```

This client has no web framework integration, so **nothing is timed automatically**: you choose
what to wrap. Keep transaction names low-cardinality (`"GET /users/:id"`, not `"GET /users/42"`):
every distinct name is its own row. `ScopedTransaction`'s destructor runs during stack unwinding,
so a scope that throws is still timed and the exception propagates unchanged. Turn it off with
`track_performance = false`; it also does nothing (and starts no thread) when reporting isn't
enabled for the current environment.

The first recorded duration starts one background `std::thread` that flushes on the interval (the
same lazy start `DeliveryQueue` uses), waiting on a condition variable rather than sleeping so it
stops promptly. What's left is flushed when the process exits normally (the flusher's destructor,
the C++ equivalent of the Ruby gem's `at_exit`); a process that exits some other way (a fatal
signal, `_exit`) loses the last window. `forge_ops_tracker::flush_performance()` sends it right now.

A failed delivery keeps every tally, so the next flush's window just grows. What a flush delivered
is *subtracted* from the tallies afterward, never the whole map cleared: a `record_performance` call
from another thread that lands while the network call is in flight (the lock is deliberately
released around it) would otherwise be silently discarded, a real bug `sdks/go` had and fixed and
that `gems/forge_ops_tracker`'s reference implementation still has. A deterministic test pins this.

## Distributed tracing

A slow call's own breakdown: which database calls, HTTP calls, or pieces of your code the time went
to, shown as a span tree on ForgeOps. Wrap the unit of work in a `ScopedTrace`, and anything inside
it, on the same thread, can add spans; the trace is sent only when the whole thing took at least
`Configuration::trace_capture_threshold` (1 second by default), so fast calls cost nothing on the
wire. A trace can also be followed into the services you call and continued from the service that
called you (see "Following a request across services" below).

```cpp
{
    forge_ops_tracker::ScopedTrace trace("GET /checkout");

    forge_ops_tracker::ScopedSpan load("load order", "database", {{"order_id", 42}});
    auto order = repo.find(42);

    forge_ops_tracker::span("charge card", "service", [&] { return gateway.charge(order); });

    // Something you timed yourself (kind is one of controller/service/database/redis/http/job/other):
    forge_ops_tracker::record_span("SELECT orders", "database", started_at, duration_ms);
}
```

`kind` outside that list is sent as `other`, since the server rejects a whole trace over one unknown
kind. This client has no web framework integration, so **nothing starts a trace or records a span
automatically**: you wrap what you want traced. Both RAII types record from their destructor, so a
scope that throws is still recorded and sent. A `ScopedSpan` nests under whichever span is open on
the same thread and does nothing outside a trace; a `ScopedTrace` inside an open trace records a span
instead of starting a second trace. The open trace is a `thread_local`, like the breadcrumb trail, so
it belongs to the thread that started it. A trace holds at most 500 spans.

Delivery runs on one background `std::thread` fed by a bounded queue (`Configuration::queue_size`),
started on the first finished slow trace and drained at normal exit; a full queue drops the trace
rather than blocking the caller. Turn span reporting off with `track_tracing = false`.

### Database spans with their SQL

A `database` span can carry the SQL it ran (a query against an embedded SQLite database, say) and
which database it was. Every string and number literal is replaced by `?` before it leaves your
process (so `WHERE email = 'a@b.co'` is sent as `WHERE email = ?`), the statement is cut at 4000
characters, and ForgeOps masks it again on arrival. It is sent in the span's data as `db.statement`
and `db.system`, and ForgeOps shows it on the span. Both are ignored on any other kind. This client
doesn't instrument a database library itself, so pass the statement where you run the query:

```cpp
const std::string sql = "SELECT * FROM readings WHERE sensor_id = 42 AND label = 'boiler'";
{
    forge_ops_tracker::ScopedSpan query("Load readings", "database");
    query.set_statement(sql, "sqlite");
    sqlite3_exec(db, sql.c_str(), on_row, nullptr, nullptr);
}
// Sent as db.statement "SELECT * FROM readings WHERE sensor_id = ? AND label = ?", db.system "sqlite".

// Or around a callable, or for a query you timed yourself:
auto rows = forge_ops_tracker::database_span("Load readings", sql, "sqlite", [&] { return load(sql); });
forge_ops_tracker::record_database_span("Load readings", started_at, duration_ms, sql, "sqlite");
```

A `db.statement` you put in the `data` of any `database` span is masked the same way.

### Following a request across services

Traces use the [W3C Trace Context](https://www.w3.org/TR/trace-context/) standard (a `traceparent`
header), so an error or a slow call can be followed from one service into the next.

**Outgoing**: wrap each HTTP call you make inside a trace in a `ScopedHttpSpan` (or `http_span()`).
It records the call as an `http` span named after the method and host (never the path or query) and
hands back the `traceparent` header value to send; its parent id is that span's own id, so the
called service's spans nest under it:

```cpp
{
    forge_ops_tracker::ScopedTrace trace("POST /checkout");

    forge_ops_tracker::ScopedHttpSpan http("POST", url);
    struct curl_slist* headers = nullptr;
    if (http.traceparent()) {
        headers = curl_slist_append(headers, ("traceparent: " + *http.traceparent()).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    CURLcode result = curl_easy_perform(curl);
    curl_slist_free_all(headers);
}

// Or with a callable, for any HTTP client:
auto response = forge_ops_tracker::http_span("GET", url, [&](const std::optional<std::string>& traceparent) {
    if (traceparent) request.set_header(forge_ops_tracker::trace_parent::header, *traceparent);
    return client.send(request);
});
```

`traceparent()` is `std::nullopt` outside a trace (where nothing is recorded either), with
`propagate_traces = false`, or when the URL's host isn't in the propagation targets below. The span
is recorded from the destructor, so a call that throws is still recorded. This client doesn't
instrument libcurl or any other HTTP client itself, so a call made without an http span carries no
header.

**Incoming**: pass the request's own `traceparent` header to `ScopedTrace` (or `trace()`) and it
continues the caller's trace (same trace id, root span parented under the caller's span).
`std::nullopt` or a malformed value just starts a new trace:

```cpp
// However your server exposes the incoming request's headers, as a std::optional<std::string>:
std::optional<std::string> traceparent = request.header("traceparent");
{
    forge_ops_tracker::ScopedTrace trace("POST /orders", traceparent);
    handle(request);
}
```

Every error captured inside a trace (`capture_exception` and friends, or the terminate handler on
that thread) carries that trace's id (`forge_ops_tracker::current_trace_id()` returns it too, for your
own logs), so ForgeOps can show it next to errors from the other services that handled the same
request. Errors captured outside a trace are unchanged. The trace id and the header exist even with
`track_tracing = false`, since they are also what links errors across services; only span reporting
stops.

The service on the other end must also report to ForgeOps (the Ruby SDK continues the trace
automatically from 0.12.0), and both projects must be linked in ForgeOps to see them connected.

Narrow or turn off where the header goes, for example if a third-party API rejects unknown headers:

```cpp
forge_ops_tracker::init([](forge_ops_tracker::Configuration& c) {
    // std::nullopt (the default) means every host. A host string matches that host and its
    // subdomains ("example.com" matches "api.example.com", never "badexample.com"); a std::regex
    // is searched for anywhere in the host.
    c.trace_propagation_targets = std::vector<forge_ops_tracker::TracePropagationTarget>{
        "example.com",
        std::regex(R"(^svc-\d+\.internal$)"),
    };
    // Or never send it at all (default true):
    c.propagate_traces = false;
});
```

## Custom metrics and infrastructure monitoring

Two explicit calls (nothing is automatic, so there is no `track_metrics` flag): a business event you
name yourself, and a reading from one of your own hosts.

```cpp
forge_ops_tracker::capture_metric("signup");          // value defaults to 1.0: a bare counter
forge_ops_tracker::capture_metric("payment", 49.0);   // a real magnitude; it may be negative (a refund)

forge_ops_tracker::capture_infrastructure_metric("cpu", 0.42);        // hostname defaults to server_name
forge_ops_tracker::capture_infrastructure_metric("disk", 0.81, "db-1");
forge_ops_tracker::flush_metrics();                                   // send right now
```

Each capture is buffered and flushed as one batch every `Configuration::metric_flush_interval` /
`infrastructure_metric_flush_interval` (60 seconds by default) on a `std::thread` started at the first
capture, and once more when the process exits normally (the buffers are globals, and a global's
destructor runs at exit), so a short-lived cron program that captures a few readings and returns from
`main` needs nothing more; call `flush_metrics()` if it might exit another way (`std::_Exit`, a signal).
Every entry is stored as it was captured (a signup is a row, not a running total), so a count or sum
you compute later is exact. Both are a no-op when reporting isn't enabled for the environment.

A failed delivery keeps every entry for the next flush, and an entry captured while a delivery is in
flight is kept too (the Ruby gem's own buffer loses it; a test pins this with a hook that captures at
exactly that moment). Each buffer holds at most 1000 entries and drops further ones until a flush
succeeds, since a plan without the feature rejects every flush and would otherwise grow it for as long
as the process lives. A NaN or infinite value is dropped at capture: `nlohmann::json` serializes it as
`null`, which the server would reject along with the whole batch behind it. Requires a ForgeOps plan
that includes custom metrics / infrastructure monitoring.

## Recording changes

Tell ForgeOps when something changed outside a release (a feature flag flipped, a config value
changed, a firmware setting pushed to a device) so it shows up next to the errors and slowdowns that
followed:

```cpp
#include <forge_ops_tracker/forge_ops_tracker.hpp>

int main() {
    forge_ops_tracker::init([](forge_ops_tracker::Configuration& config) {
        config.dsn = "https://<api_key>@getforgeops.net/api/v1/events";
        config.environment = "production";
    });

    forge_ops_tracker::record_change("feature_flag", "Enabled new checkout", {{"flag", "new_checkout"}, {"to", true}});

    forge_ops_tracker::ChangeOptions options;
    options.actor = "deploy-bot";
    options.url = "https://example.com/pr/42";
    forge_ops_tracker::record_change("config", "Raised the upload limit", nlohmann::json::object(), options);
    return 0; // anything still queued is delivered as the process exits normally
}
```

`kind` is one of `feature_flag`, `config`, `migration`, `dependency`, `infrastructure` or `other`
(`forge_ops_tracker::change_kinds`); anything else is sent as `other`. The title is cut to 200
characters (never through a multibyte UTF-8 character), and a blank one records nothing. `details` is
sent when it is a non-empty JSON object. `ChangeOptions` is all optional: `environment` defaults to
`Configuration::environment`, `occurred_at` to now, and `service`, `actor`, `url` and `id` (your own
idempotency key, so a retried call records the change once) are left out when unset.

Delivery runs on a background thread fed by the same kind of bounded queue traces use, so the call
never blocks on the network, and the queue is drained when the process exits normally. It never
throws: a full queue or a failed delivery (including the 403 a plan without change tracking returns)
drops the change quietly. It is a no-op when reporting isn't enabled for the environment. This client
runs on devices and embedded targets as well as servers, so it sends no automatic startup snapshot;
every change is one you record.

## How delivery works: a bounded queue, drained by a background thread

Every report is pushed onto a `DeliveryQueue` (bounded by `Configuration::queue_size`, default
1000) and handed off to a single background `std::thread` that drains it one item at a time over
HTTP via `Client` (libcurl). `push()` itself never blocks the calling thread waiting on a network
call (it only briefly holds a mutex to enqueue) so reporting an error from a request-handling
thread can't stall that thread on a slow or unreachable tracker. If the queue is already at
capacity when `push()` is called (the background worker is behind, typically because delivery is
slow or the tracker is unreachable), the new event is dropped and logged rather than blocking or
growing unbounded; `Configuration::logger`, if set, is called with a message describing the drop.

The worker thread is started lazily, on the first `push()`, not at construction. A C++ binary has
no interpreter-level "module load" moment a prefork server could fork after, so there's no
fork-safety concern driving this; it's simply so a `Reporter` that's constructed but never actually
used to report anything never spins up a thread it doesn't need.

Destroying a `DeliveryQueue` (its destructor) signals the worker to stop and joins it, but the
worker's own loop keeps draining whatever's still queued before actually returning, rather than
abandoning it mid-queue. That means destruction can block for as long as delivering the remaining
items takes (up to `Configuration::timeout_seconds` each, if the tracker is slow or unreachable):
a real, worth-knowing behavior, not a bug: it trades "might block briefly at shutdown" for "don't
silently drop everything still in flight when the process is tearing down normally." Confirmed
directly by a real test (`delivery_queue_destructor_drains_pending_items_before_returning`), not
just read off the source.

## Backtrace frames: image + symbol, never file/line, captured where you catch, not where you throw

`backtrace()`/`backtrace_symbols()` (`<execinfo.h>`) give a binary image name (closest available
analog to "file") and a resolved, demangled symbol (closest analog to "method"); `line` is always
JSON `null`: a compiled, optimized C++ binary has no source file/line information left in it at
runtime.

The backtrace is captured at the moment `build()` (or `capture_exception`/`report`) is *called*,
not at the moment the exception was originally thrown: C++ exceptions carry no stack trace of
their own. Confirmed directly on this machine: by the time a `catch` block runs, the stack has
typically already unwound back up through most of the intervening call frames, so a real backtrace
captured from inside a `catch` block is usually much shorter than the call stack that actually led
to the `throw`. Call `capture_exception`/`report` as close to the `catch` site as possible for the
most useful trace.

`in_app` compares a frame's image name against the running process's own executable name
(`_NSGetExecutablePath` on macOS, via `<mach-o/dyld.h>`): true only for frames inside your own
binary, `false` for anything from a shared library. Since this test suite links the library
statically into its own test binary, every frame is technically "your own binary" in that build:
a real caveat for any consumer who does the same (link statically rather than against a `.so`/
`.dylib`), documented here rather than silently left as a surprise: `in_app` degrades to "always
true" when the library and the app share one binary image, the same limitation `is_in_app`'s own
comment in `event_builder.cpp` already flags.

## Source context

`Configuration::capture_source_context` exists (defaulting to `true`, the same default every other
SDK in this repo uses) purely for API-shape consistency: a host app configuring this client sees
the same option every other SDK has. It does nothing here. Every other SDK in this repo that
supports it reads a few lines of source off disk around an in-app frame's culprit line at
capture-time, keyed off that frame's own file path and line number, but a backtrace frame here
never carries a real file path or line number at all (see "Backtrace frames" above: `line` is
always JSON `null`, since a compiled, optimized C++ binary has no source location left in it at
runtime). `EventBuilder::attach_source_context` is a documented no-op rather than a partial
implementation of something that can never actually run: there is no case, on this client's
capture path, where a real file+line pair exists to read.

## PII scrubbing

PII pattern matching uses `std::regex`'s default ECMAScript grammar, which accepts all 8 patterns
(including `\b` word boundaries) as written; verified directly against real matching input for
each pattern (see the test file's own `pii_*` tests), not assumed to translate cleanly just
because the syntax looks familiar.

The message, backtrace, and any context you attach are scanned for likely personal data: email
addresses, formatted SSNs/credit cards, known API key/token formats (AWS, Stripe, GitHub, JWT,
bearer tokens), and anything under a suspiciously-named key (`password`, `api_key`, `ssn`, and
similar, matched case- and punctuation-insensitively) and redacted before the payload ever
leaves this process. ForgeOps itself scrubs again on arrival regardless, so this is a second,
earlier layer, not the only one. Deliberately does *not* support `Project#additional_sensitive_keys`
(server-side only, by design: see `pii_scrubber.hpp`'s own header comment). The user attached via
`capture_exception`'s `user` argument or `set_user` above is a deliberate exception: it's never
scrubbed, since redacting it would defeat the whole point of identifying users in the first place.

To disable it: `config.scrub_pii = false;`

## Database errors

C++ exceptions carry no SQL of their own, so the code that ran the query hands it over, in either of two ways. Throw (or rethrow from a catch of the driver's own exception) a `forge_ops_tracker::SqlException`, or report an exception you already have with `capture_exception_with_sql`. Either way the event includes the names of the stored procedure, table and view that SQL touched, so the issue tells you where to start looking. Names are identifiers, never values; the raw statement never leaves the process.

To also send the SQL statement itself, opt in. Every string and number is replaced by `?` before it
leaves your process (`WHERE email = 'a@b.co' AND id = 42` is sent as `WHERE email = ? AND id = ?`),
and ForgeOps masks it again on arrival:

```cpp
try {
    session << query, soci::use(id);
} catch (const std::exception& e) {
    forge_ops_tracker::capture_exception_with_sql(e, query);
    throw;
}

// Or attach it to the exception itself:
//   throw forge_ops_tracker::SqlException(e.what(), query);

// Opt in to also sending the masked statement (default false). capture_sql_objects (default true)
// controls the names.
forge_ops_tracker::init([](forge_ops_tracker::Configuration& c) { c.capture_sql_statement = true; });
```

Each ForgeOps project also has its own "Capture the SQL behind database errors" setting. Turn it off
there and the statement is never stored for that project, whatever this flag says; the names are
still kept. A view and a table are written the same way in SQL, so both show as tables/views; the
database's own error message usually settles which it was.

## Running the tests

```bash
cd sdks/cpp
cmake -B build
cmake --build build
./build/tests/forge_ops_tracker_tests
# or: cd build && ctest --output-on-failure
```

A hand-rolled test runner (`TEST`/`RUN`/`ASSERT_TRUE` macros), not an external framework like
Catch2 or GoogleTest: a real decision, not an oversight. Reaching for a vendored test framework
purely for the tests themselves, on top of the two real runtime dependencies this library already
has (libcurl, nlohmann_json: see "Dependencies" above), would add a third dependency for no
capability the ~80 lines of macros at the top of the test file don't already provide. C++
exceptions make the hand-rolled approach straightforward here: `RUN()` wraps each test in a
`try`/`catch` so an assertion failure (or any unexpected exception) fails just that one test and
moves on.

The test suite covers every component with real assertions, including real local HTTP delivery
(a small `TestServer` helper spins up a background-thread HTTP server on `127.0.0.1`, reading the
*full* request before responding to avoid racing curl's own write) and a real bounded-queue drop
test against a deliberately non-responding "black hole" listener. All 34 tests pass under both a
plain build and a build with AddressSanitizer + UndefinedBehaviorSanitizer
(`-fsanitize=address,undefined`): zero sanitizer findings.

The terminate handler's own body isn't exercised by this test suite beyond confirming installation
is idempotent: deliberately: actually letting an exception escape uncaught to trigger it for real
would terminate the test process itself.

### A real bug this test suite caught

Both convenience overloads that accept an already-caught exception by reference
(`EventBuilder::build(const std::exception&, ...)` and `Reporter::report(const std::exception&,
...)`) originally rebuilt an `exception_ptr` via `std::make_exception_ptr(exception)`. That looks
correct but silently slices: template argument deduction for `make_exception_ptr` picks its
template parameter from `exception`'s *declared* parameter type (`const std::exception&`), not its
real dynamic type, so a caught custom exception (say, a `std::runtime_error` subclass with its own
message) came back through the rebuilt `exception_ptr` as a bare `std::exception`: both its real
class name and its own `what()` message replaced with `std::exception`'s generic boilerplate text.
Verified directly with a standalone repro before touching the fix (a derived exception's message
came back as the literal string `"std::exception"`, not the message it was actually constructed
with), then caught again by this SDK's own `event_builder_basic_fields` and
`event_builder_scrubs_message_and_context_by_default` tests, which failed against the *unfixed*
source with exactly that symptom before the fix below was applied.

The fix: both functions now prefer `std::current_exception()`, which has no such problem: it
reflects whatever's genuinely in flight, and calling it from inside the same `catch` block that
caught the exception (even through another function call, like `capture_exception` calling into
`Reporter::report`) still counts as "being handled" per the standard. Both fall back to the
original `make_exception_ptr(exception)` behavior only when nothing is actually in flight (a caller
building/reporting a `std::exception` object it never threw): that edge case genuinely can't
recover a derived type through a plain base-class reference; no implementation can work around
that, it's a hard C++ limitation rather than a bug in this one. See the comments on both fixed
functions (`event_builder.cpp`, `reporter.cpp`) for the full reasoning inline.

This means anyone who had already built against the *original* unfixed source and relied on
`capture_exception(e)`/`report(e)` from inside a real `catch (const std::exception& e)` block was
silently getting `"std::exception"` as both the exception class and the message for every report
sent that way: worth knowing if this SDK is ever pulled from a point before this fix.
