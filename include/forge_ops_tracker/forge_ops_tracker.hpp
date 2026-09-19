#pragma once

#include <exception>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/configuration.hpp"
#include "forge_ops_tracker/span_buffer.hpp"

namespace forge_ops_tracker {

/** Configure the client. Call once at startup. `configure` receives a mutable reference to apply overrides to. */
Configuration& init(const std::function<void(Configuration&)>& configure = nullptr);

/**
 * Report an exception you've already caught. `user` defaults to whatever set_user last
 * established on this thread, if anything; pass one explicitly to override that for this one
 * report.
 */
void capture_exception(const std::exception_ptr& exception_ptr, const nlohmann::json& context = nlohmann::json::object(), const nlohmann::json& user = nlohmann::json::object());
void capture_exception(const std::exception& exception, const nlohmann::json& context = nlohmann::json::object(), const nlohmann::json& user = nlohmann::json::object());

/**
 * Manually attaches an affected user to whatever gets reported from here on, *on this thread* (an
 * explicit capture_exception call with no `user` argument, or whatever the installed terminate
 * handler reports): there's no way to automatically detect "the current user" the way a
 * server-side web framework with its own session/auth middleware can, so call this yourself, e.g.
 * right after authenticating a request. A plain thread_local, not a process-wide global: the
 * right choice for a server handling more than one request at a time, each on its own thread, the
 * same reasoning gems/forge_ops_tracker documents for its own Thread.current use; a
 * single-threaded program just has the one thread's worth of state, so this still behaves like a
 * plain global there. Pass an empty object to clear whatever was set, e.g. once a request
 * finishes or on sign-out.
 */
void set_user(const nlohmann::json& user);

/**
 * Records one breadcrumb: an entry in a small, bounded trail of recent events attached to
 * whatever gets reported next *on this thread* (an explicit capture_exception call, or whatever
 * the installed terminate handler reports), so an issue's detail page can show what led up to it.
 * `category`/`level` default to "custom"/"info"; `data` is any small amount of extra structured
 * detail. Only the Configuration::max_breadcrumbs most recent are kept, oldest dropped first;
 * does nothing when Configuration::track_breadcrumbs is false.
 *
 * Nothing records one automatically: this client has no web framework integration to time a
 * request from, so every breadcrumb here is one you add by hand, wherever it's meaningful. A
 * plain thread_local, the same choice set_user makes and for the same reason (a server handling
 * one request per thread gets isolation for free), so a thread that serves more than one unit of
 * work in a row (a thread pool worker, say) must call clear_breadcrumbs() itself at the start of
 * each one, or the previous unit's trail carries over.
 */
void add_breadcrumb(const std::string& message, const std::string& category = "custom", const std::string& level = "info", const nlohmann::json& data = nlohmann::json::object());

/** Empties this thread's breadcrumb trail: see add_breadcrumb for when to call it. */
void clear_breadcrumbs();

/**
 * Records one timed call's duration, in milliseconds, under `transaction_name`: tallied in-process
 * (count, total, max) and flushed periodically (Configuration::performance_flush_interval, 60s by
 * default) as one small aggregate report per transaction, for the Performance page's
 * per-transaction table, not one network call per call. A no-op when
 * Configuration::track_performance is false or reporting isn't enabled for this environment.
 *
 * This client has no web framework integration, so nothing is timed automatically: wrap whatever
 * you want on the Performance page yourself, with ScopedTransaction or time_transaction below, or
 * call this directly with a duration you measured. Keep `transaction_name` low-cardinality
 * ("GET /users/:id", not "GET /users/42"): every distinct name is its own row. The first recorded
 * duration starts one background std::thread that flushes on the interval (the same lazy start
 * DeliveryQueue uses); the last partial window is flushed when the process exits normally.
 */
void record_performance(const std::string& transaction_name, double duration_ms);

/**
 * RAII timer: records how long it lived, under `transaction_name`, when it goes out of scope. The
 * natural C++ shape for this, and the reason exceptions need no special handling: the destructor
 * runs during unwinding, so a scope that throws is still timed, and a handler that fails is
 * exactly one worth seeing on the Performance page.
 *
 *     {
 *         forge_ops_tracker::ScopedTransaction timing("GET /users/:id");
 *         handle_request(request);
 *     }
 */
class ScopedTransaction {
public:
    explicit ScopedTransaction(std::string transaction_name)
        : transaction_name_(std::move(transaction_name)), started_at_(std::chrono::steady_clock::now()) {}

    ~ScopedTransaction() {
        std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - started_at_;
        record_performance(transaction_name_, elapsed.count());
    }

    ScopedTransaction(const ScopedTransaction&) = delete;
    ScopedTransaction& operator=(const ScopedTransaction&) = delete;

private:
    std::string transaction_name_;
    std::chrono::steady_clock::time_point started_at_;
};

/** Runs `f`, records how long it took under `transaction_name` (see ScopedTransaction), and returns whatever `f` returned. */
template <typename F>
auto time_transaction(const std::string& transaction_name, F&& f) -> decltype(f()) {
    ScopedTransaction timing(transaction_name);
    return f();
}

/** Delivers whatever has been tallied so far right now, instead of waiting for the next interval. Blocks on libcurl for up to Configuration::timeout_seconds. */
void flush_performance();

/**
 * Custom metrics and infrastructure monitoring: two explicit calls (nothing is automatic, so there is
 * no track_metrics flag). capture_metric records a named business event (a signup, a payment, anything
 * you want to name): pass 1.0 for a bare counter or a real magnitude, and it may be negative (a
 * refund). capture_infrastructure_metric records one reading (cpu, memory, disk, anything else a
 * program of yours reads) from one of your own hosts; an empty hostname means
 * Configuration::server_name, so a script on the box it reports about needs none. Both are buffered
 * and flushed as one batch every Configuration::metric_flush_interval /
 * infrastructure_metric_flush_interval (60s) on a background std::thread started at the first capture,
 * and flushed once more when the process exits normally (a global's destructor), which is what a
 * short-lived cron program that captures a few readings and returns from main relies on; call
 * flush_metrics() if it might exit another way (std::_Exit, a signal). Every entry is stored as
 * captured (a signup is a row, not a running total), so a count or sum computed later is exact. Both
 * are a no-op when reporting isn't enabled for this environment, and a NaN or infinite value is
 * dropped. A buffer holds at most MetricBuffer::max_entries entries and drops further ones until a
 * flush succeeds. A failed delivery keeps every entry, and one captured while a delivery is in flight
 * is kept too.
 */
void capture_metric(const std::string& name, double value = 1.0);
void capture_infrastructure_metric(const std::string& name, double value, const std::string& hostname = "");

/** Delivers every buffered metric and infrastructure reading right now. Blocks on libcurl for up to Configuration::timeout_seconds. */
void flush_metrics();

/**
 * Distributed tracing: one request's or job's own call tree, sent to ForgeOps only when the whole
 * thing took at least Configuration::trace_capture_threshold (1 second by default), so fast calls
 * cost nothing on the wire. Wrap the unit of work in a ScopedTrace (or trace()), and anything inside
 * it, on the same thread, can add spans with ScopedSpan (or span()); a span nests under whichever
 * span is open. Traces are per service: nothing is propagated across services.
 *
 *     {
 *         forge_ops_tracker::ScopedTrace trace("GET /checkout");
 *         forge_ops_tracker::ScopedSpan charge("charge card", "service", {{"order_id", 42}});
 *         gateway.charge(order);
 *     }
 *
 * `kind` is one of controller, service, database, redis, http, job, other (anything else is sent as
 * "other", since the server rejects a whole trace over one unknown kind). Outside a trace every
 * call is a harmless no-op, as is everything when Configuration::track_tracing is false or reporting
 * isn't enabled for this environment. Both RAII types record from their destructor, so a scope that
 * throws is still recorded and sent.
 *
 * This client has no web framework integration, so nothing starts a trace or records a span
 * automatically: you wrap what you want traced. A ScopedTrace inside an open trace does not start a
 * second one; it records a span instead. The open trace is a thread_local, like the breadcrumb
 * trail, so it belongs to the thread that started it. Delivery runs on one background std::thread
 * fed by a bounded queue, drained at normal exit; a full queue drops the trace rather than blocking.
 */
class ScopedSpan {
public:
    ScopedSpan(std::string name, std::string kind = "service", nlohmann::json data = nlohmann::json::object());
    ~ScopedSpan();

    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;

private:
    std::string name_;
    std::string kind_;
    nlohmann::json data_;
    std::string span_id_; // empty outside a trace: nothing to record
    std::chrono::system_clock::time_point started_at_;
    std::chrono::steady_clock::time_point timer_;
};

class ScopedTrace {
public:
    explicit ScopedTrace(std::string root_name);
    ~ScopedTrace();

    ScopedTrace(const ScopedTrace&) = delete;
    ScopedTrace& operator=(const ScopedTrace&) = delete;

private:
    std::string root_name_;
    std::chrono::system_clock::time_point started_at_;
    std::chrono::steady_clock::time_point timer_;
    bool owns_trace_;
    std::optional<ScopedSpan> nested_; // set when this was nested inside an open trace
};

/** Records a span you timed yourself under the current one; a no-op outside a trace. */
void record_span(const std::string& name, const std::string& kind, std::chrono::system_clock::time_point started_at, double duration_ms, const nlohmann::json& data = nlohmann::json::object());

/** Runs `f` as a trace named `root_name` (see ScopedTrace) and returns whatever `f` returned. */
template <typename F>
auto trace(const std::string& root_name, F&& f) -> decltype(f()) {
    ScopedTrace scoped(root_name);
    return f();
}

/** Runs `f` as a span (see ScopedSpan) and returns whatever `f` returned. */
template <typename F>
auto span(const std::string& name, const std::string& kind, F&& f) -> decltype(f()) {
    ScopedSpan scoped(name, kind);
    return f();
}

/**
 * Installs a std::terminate handler that reports whatever's in flight (if it's a real exception,
 * see the .cpp for what "in flight" can mean when std::terminate is reached some other way) before
 * chaining to the previously-installed handler. std::terminate handlers are documented as not
 * being permitted to return, so this SDK cannot resume normal execution afterward any more than
 * the default handler could: the process will still terminate, the same way it would without
 * this installed; this only adds a report on the way out.
 */
void install_terminate_handler();

/** @internal not part of the public API: resets module state between test cases */
void reset_for_testing();

} // namespace forge_ops_tracker
