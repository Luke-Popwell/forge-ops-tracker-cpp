#include "forge_ops_tracker/forge_ops_tracker.hpp"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <memory>

#include "forge_ops_tracker/client.hpp"
#include "forge_ops_tracker/delivery_queue.hpp"
#include "forge_ops_tracker/metric_buffer.hpp"
#include "forge_ops_tracker/performance_flusher.hpp"
#include "forge_ops_tracker/reporter.hpp"
#include "forge_ops_tracker/span_queue.hpp"

namespace forge_ops_tracker {

namespace {

std::unique_ptr<Configuration> g_configuration;
std::unique_ptr<DeliveryQueue> g_delivery_queue;
std::unique_ptr<Reporter> g_reporter;
std::unique_ptr<PerformanceFlusher> g_performance_flusher;
std::unique_ptr<SpanQueue> g_span_queue;
std::unique_ptr<MetricBuffer> g_metric_buffer;
std::unique_ptr<MetricBuffer> g_infrastructure_metric_buffer;
std::terminate_handler g_previous_terminate_handler = nullptr;
bool g_terminate_handler_installed = false;

// The user set via set_user, if any. See set_user's own header comment for why this is
// thread_local rather than a plain global.
thread_local nlohmann::json g_current_user = nlohmann::json::object();

// This thread's breadcrumb trail. See add_breadcrumb's own header comment for why this is
// thread_local too. A json array of {category,message,level,timestamp,data} objects, matching
// the wire shape directly, the same "everything here is already json" choice g_current_user makes.
thread_local nlohmann::json g_current_breadcrumbs = nlohmann::json::array();

// This thread's open trace, if any. See the tracing doc comment in forge_ops_tracker.hpp for why
// this is thread_local too.
thread_local std::unique_ptr<SpanBuffer> g_trace;

Configuration& configuration() {
    if (!g_configuration) {
        g_configuration = std::make_unique<Configuration>();
    }
    return *g_configuration;
}

Reporter& reporter() {
    if (!g_reporter) {
        g_delivery_queue = std::make_unique<DeliveryQueue>(configuration(), Client(configuration()));
        g_reporter = std::make_unique<Reporter>(configuration(), EventBuilder(configuration()), *g_delivery_queue);
    }
    return *g_reporter;
}

PerformanceFlusher& performance_flusher() {
    if (!g_performance_flusher) {
        g_performance_flusher = std::make_unique<PerformanceFlusher>(configuration(), Client(configuration()));
    }
    return *g_performance_flusher;
}

void ensure_metric_buffers() {
    if (!g_metric_buffer) {
        Configuration& config = configuration();
        auto client = std::make_shared<Client>(config);
        g_metric_buffer = std::make_unique<MetricBuffer>(
            config, [client](const nlohmann::json& entries) { return client->deliver_metrics(entries); },
            [&config] { return config.metric_flush_interval; });
        g_infrastructure_metric_buffer = std::make_unique<MetricBuffer>(
            config, [client](const nlohmann::json& entries) { return client->deliver_infrastructure_metrics(entries); },
            [&config] { return config.infrastructure_metric_flush_interval; });
    }
}

std::optional<std::string> open_trace_id() {
    return g_trace ? std::optional<std::string>(g_trace->trace_id()) : std::nullopt;
}

SpanQueue& span_queue() {
    if (!g_span_queue) {
        g_span_queue = std::make_unique<SpanQueue>(configuration(), Client(configuration()));
    }
    return *g_span_queue;
}

void handle_terminate() {
    // std::current_exception() is well-defined to return whatever exception caused termination
    // when std::terminate() was reached via an uncaught exception: per the standard, not
    // assumed, and returns a null exception_ptr otherwise (terminate() called directly, a
    // noexcept violation with nothing currently propagating, and so on), which report() below
    // already handles as "no exception_class/message available" the same way Reporter's
    // catch (...) branch does.
    std::exception_ptr current = std::current_exception();
    if (current) {
        reporter().report(current, nlohmann::json::object(), g_current_user, g_current_breadcrumbs, "", open_trace_id());
    }

    if (g_previous_terminate_handler) {
        g_previous_terminate_handler();
    }
    // A terminate handler is documented as not permitted to return: fall through to the same
    // default behavior the process would have had without this installed, rather than returning
    // and leaving the process in an undefined state.
    std::abort();
}

} // namespace

Configuration& init(const std::function<void(Configuration&)>& configure) {
    Configuration& config = configuration();
    if (configure) {
        configure(config);
    }
    return config;
}

void capture_exception(const std::exception_ptr& exception_ptr, const nlohmann::json& context, const nlohmann::json& user) {
    reporter().report(exception_ptr, context, user.empty() ? g_current_user : user, g_current_breadcrumbs, "", open_trace_id());
}

void capture_exception(const std::exception& exception, const nlohmann::json& context, const nlohmann::json& user) {
    reporter().report(exception, context, user.empty() ? g_current_user : user, g_current_breadcrumbs, "", open_trace_id());
}

void capture_exception_with_sql(const std::exception& exception, const std::string& sql, const nlohmann::json& context, const nlohmann::json& user) {
    reporter().report(exception, context, user.empty() ? g_current_user : user, g_current_breadcrumbs, sql, open_trace_id());
}

void set_user(const nlohmann::json& user) {
    g_current_user = user;
}

void add_breadcrumb(const std::string& message, const std::string& category, const std::string& level, const nlohmann::json& data) {
    const Configuration& config = configuration();
    if (!config.track_breadcrumbs) {
        return;
    }

    // gmtime_r, not std::gmtime: std::gmtime returns a pointer to one shared static buffer, which
    // two threads adding a breadcrumb at once would race on. This is the one place in this client
    // a timestamp is formatted from more than one thread at a time by design (every thread has its
    // own trail).
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);

    g_current_breadcrumbs.push_back({
        {"category", category},
        {"message", message},
        {"level", level},
        {"timestamp", timestamp},
        {"data", data},
    });

    while (g_current_breadcrumbs.size() > config.max_breadcrumbs) {
        g_current_breadcrumbs.erase(g_current_breadcrumbs.begin());
    }
}

void clear_breadcrumbs() {
    g_current_breadcrumbs = nlohmann::json::array();
}

void record_performance(const std::string& transaction_name, double duration_ms) {
    performance_flusher().record(transaction_name, duration_ms);
}

void flush_performance() {
    if (g_performance_flusher) {
        g_performance_flusher->flush();
    }
}

void capture_metric(const std::string& name, double value) {
    const Configuration& config = configuration();
    if (!config.is_enabled()) {
        return;
    }
    ensure_metric_buffers();
    g_metric_buffer->record({
        {"metric_name", name},
        {"value", value},
        {"environment", config.environment},
        {"release", config.release ? nlohmann::json(*config.release) : nlohmann::json(nullptr)},
    });
}

void capture_infrastructure_metric(const std::string& name, double value, const std::string& hostname) {
    const Configuration& config = configuration();
    if (!config.is_enabled()) {
        return;
    }
    ensure_metric_buffers();
    g_infrastructure_metric_buffer->record({
        {"metric_name", name},
        {"value", value},
        {"hostname", !hostname.empty() ? hostname : config.server_name.value_or("")},
    });
}

void flush_metrics() {
    if (g_metric_buffer) {
        g_metric_buffer->flush();
        g_infrastructure_metric_buffer->flush();
    }
}

ScopedSpan::ScopedSpan(std::string name, std::string kind, nlohmann::json data)
    : name_(std::move(name)),
      kind_(std::move(kind)),
      data_(std::move(data)),
      started_at_(std::chrono::system_clock::now()),
      timer_(std::chrono::steady_clock::now()) {
    if (g_trace) {
        span_id_ = g_trace->open();
    }
}

ScopedSpan::~ScopedSpan() {
    if (span_id_.empty() || !g_trace) {
        return;
    }
    try {
        std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - timer_;
        g_trace->close(span_id_, name_, kind_, started_at_, elapsed.count(), data_);
    } catch (...) {
        // Recording must never be able to throw out of a destructor that may be running during unwinding.
    }
}

ScopedTrace::ScopedTrace(std::string root_name) : ScopedTrace(std::move(root_name), std::nullopt) {}

ScopedTrace::ScopedTrace(std::string root_name, const std::optional<std::string>& traceparent)
    : root_name_(std::move(root_name)),
      started_at_(std::chrono::system_clock::now()),
      timer_(std::chrono::steady_clock::now()),
      owns_trace_(!g_trace) {
    if (!owns_trace_) {
        nested_.emplace(root_name_, "service");
        return;
    }
    // Started whenever reporting is enabled, even with track_tracing off: its id still goes on errors
    // and outgoing headers, and the buffer just never sends its spans.
    const Configuration& config = configuration();
    if (config.is_enabled()) {
        g_trace = std::make_unique<SpanBuffer>(config, traceparent ? trace_parent::parse(*traceparent) : std::nullopt);
    }
}

ScopedTrace::~ScopedTrace() {
    if (!owns_trace_) {
        return;
    }
    std::unique_ptr<SpanBuffer> buffer = std::move(g_trace);
    g_trace.reset();
    if (!buffer) {
        return;
    }
    try {
        std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - timer_;
        if (auto payload = buffer->finish(root_name_, started_at_, elapsed.count())) {
            span_queue().push(std::move(*payload));
        }
    } catch (...) {
        // Same reasoning as ScopedSpan's destructor.
    }
}

std::string ScopedHttpSpan::name_for(const std::string& method, const std::string& url) {
    std::string name = method.empty() ? "GET" : method;
    for (char& c : name) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return name + " " + trace_parent::url_host(url).value_or("unknown");
}

// The span id exists as soon as span_ is constructed, before the call is made, which is what lets the
// header name the span the call is then recorded as.
ScopedHttpSpan::ScopedHttpSpan(const std::string& method, const std::string& url, nlohmann::json data)
    : span_(name_for(method, url), "http", std::move(data)) {
    if (span_.span_id().empty() || !g_trace) {
        return;
    }
    if (configuration().should_propagate_trace(trace_parent::url_host(url))) {
        traceparent_ = trace_parent::build(g_trace->trace_id(), span_.span_id());
    }
}

std::optional<std::string> current_trace_id() {
    return open_trace_id();
}

void record_span(const std::string& name, const std::string& kind, std::chrono::system_clock::time_point started_at, double duration_ms, const nlohmann::json& data) {
    if (g_trace) {
        g_trace->record_leaf(name, kind, started_at, duration_ms, data);
    }
}

void install_terminate_handler() {
    if (g_terminate_handler_installed) {
        return;
    }
    g_terminate_handler_installed = true;
    g_previous_terminate_handler = std::set_terminate(&handle_terminate);
}

void reset_for_testing() {
    // Before the configuration is destroyed (the flusher's thread reads it), and discarded rather
    // than merely destroyed: a destroyed flusher delivers whatever is left, which a test that just
    // wants a clean slate must not do.
    if (g_performance_flusher) {
        g_performance_flusher->discard();
        g_performance_flusher.reset();
    }
    if (g_metric_buffer) {
        g_metric_buffer->discard();
        g_infrastructure_metric_buffer->discard();
        g_metric_buffer.reset();
        g_infrastructure_metric_buffer.reset();
    }
    if (g_span_queue) {
        g_span_queue->discard();
        g_span_queue.reset();
    }
    g_trace.reset();
    g_reporter.reset();
    g_delivery_queue.reset();
    g_configuration.reset();
    g_current_user = nlohmann::json::object();
    g_current_breadcrumbs = nlohmann::json::array();
    // Deliberately not touching std::set_terminate here: resetting the real process-wide
    // terminate handler between test runs would risk leaving the *test binary itself* without
    // whatever handler it started with if an unrelated later test genuinely terminates.
}

} // namespace forge_ops_tracker
