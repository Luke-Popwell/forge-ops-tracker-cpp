#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace forge_ops_tracker {

/**
 * One entry in Configuration::trace_propagation_targets: a host string, matching that host and its
 * subdomains on a dot boundary ignoring case and a leading dot ("example.com" matches
 * "api.example.com", never "badexample.com"), or a std::regex searched for anywhere in the
 * lowercased host (anchor it yourself). A string literal converts to the host form.
 */
using TracePropagationTarget = std::variant<std::string, std::regex>;

/**
 * Holds a single ForgeOps DSN plus everything else the client needs to build and deliver events.
 * Mirrors gems/forge_ops_tracker/lib/forge_ops_tracker/configuration.rb: a single DSN string
 * carries both the ingestion URL and the project's api_key:
 * "https://<api_key>@host/api/v1/events".
 */
class Configuration {
public:
    std::optional<std::string> dsn;
    std::string environment = "production";
    std::optional<std::string> release;
    std::optional<std::string> server_name;

    /**
     * Used to decide whether a backtrace frame is "in_app": a frame's file compared against this
     * root. Defaults empty (meaning: never in_app): set explicitly to your app's own source
     * root, the same way the Ruby/Python/Node clients default to a real value (Rails.root,
     * process.cwd()) but this one can't guess a meaningful default the way an interpreted
     * language's own runtime can.
     */
    std::string app_root;

    std::set<std::string> enabled_environments = {"production", "staging"};
    std::size_t queue_size = 1000;
    int timeout_seconds = 2;
    bool scrub_pii = true;

    /**
     * Whether EventBuilder would read a few lines of source off disk around an in-app frame's
     * culprit line, the same way the Ruby/Python/Node/etc. clients in this repo do. Defaults on,
     * mirroring every other SDK, but this flag alone isn't the real protection against sending
     * source code somewhere it shouldn't go: ForgeOps' own per-project setting is the durable,
     * server-enforced off switch, since it applies regardless of what this flag happens to be set
     * to on any given deployment. Kept here purely for API-shape consistency across every SDK in
     * this repo: see EventBuilder's own header comment for why this specific client's capture
     * function is a documented no-op regardless of this value: backtrace_symbols() never produces
     * a real file+line pair to read in the first place.
     */
    bool capture_source_context = true;

    /**
     * When an error is reported with the SQL behind a failed database call (a SqlException, or
     * capture_exception_with_sql), send the names of the stored procedure, table and view that SQL
     * touched, so an issue says where to start looking. Names are identifiers, never values, which
     * is why this defaults on. capture_sql_statement is the separate, opt-in step of also sending
     * the statement itself, with every string and number replaced by "?"; off by default because
     * even a masked statement describes your schema, and ForgeOps' own per-project setting is what
     * durably governs whether the server stores it. See sql_statement.hpp.
     */
    bool capture_sql_objects = true;
    bool capture_sql_statement = false;

    /** Whether add_breadcrumb records anything at all. On by default, matching every other client in this repo. */
    bool track_breadcrumbs = true;

    /** How many of the most recent breadcrumbs are kept, oldest dropped first. 30, matching every other client's default. */
    std::size_t max_breadcrumbs = 30;

    /**
     * Whether record_performance/time_transaction time anything at all. On by default, the same
     * "on unless you turn it off" posture error reporting itself already has. This client has no
     * web framework integration, so nothing is timed automatically: this only gates the manual API.
     */
    bool track_performance = true;

    /**
     * How often the in-process tallies are flushed as one small aggregate report, rather than one
     * network call per timed call. Matches gems/forge_ops_tracker's own default (60s).
     */
    std::chrono::milliseconds performance_flush_interval{60000};

    /**
     * Whether a trace (ScopedTrace/trace()) is sent to ForgeOps when slow. On by default. With it
     * off, a trace still starts and has an id, attached to errors captured inside it and handed out
     * by ScopedHttpSpan (see propagate_traces), since that id is also what links an error here to one
     * in another service; only span reporting stops. This client has no web framework integration,
     * so nothing starts a trace automatically.
     */
    bool track_tracing = true;

    /**
     * How often the buffered capture_metric / capture_infrastructure_metric entries are flushed as one
     * batch (60s by default). There is no track_metrics flag the way track_performance has one: these
     * are explicit calls the host app's own code makes, not automatic instrumentation, so there is
     * nothing to turn off that simply not calling them doesn't already do.
     */
    std::chrono::milliseconds metric_flush_interval{60000};
    std::chrono::milliseconds infrastructure_metric_flush_interval{60000};

    /** A trace is only sent when its root span took at least this long. 1 second by default. */
    std::chrono::milliseconds trace_capture_threshold{1000};

    /**
     * Whether ScopedHttpSpan/http_span() hand back a W3C traceparent header for the outgoing call, so
     * the service being called continues this trace. On by default, matching gems/forge_ops_tracker:
     * the header carries the trace id that links an error here to an error there, which is useful
     * with or without spans, so it goes out even with track_tracing off.
     */
    bool propagate_traces = true;

    /**
     * Which hosts get that header. nullopt (the default) means every host; otherwise a list of
     * TracePropagationTarget (host strings and/or std::regex), and an empty list means no host.
     * Useful for a third-party API that rejects unknown headers, or that shouldn't learn your trace
     * ids at all.
     */
    std::optional<std::vector<TracePropagationTarget>> trace_propagation_targets;

    /** Whether an outgoing call to `host` should carry a traceparent header; case-insensitive, since hostnames are. */
    bool should_propagate_trace(const std::optional<std::string>& host) const;

    std::function<void(const std::string&)> logger;

    std::optional<std::string> api_key() const;

    /** The ingestion URL with credentials stripped out (they travel as the Authorization header instead). */
    std::optional<std::string> ingestion_uri() const;

    /**
     * Same derivation as ingestion_uri(), with the trailing "/events" swapped for
     * "/performance_samples": one DSN, two endpoints, matching the Ruby gem's own
     * Configuration#performance_samples_uri.
     */
    std::optional<std::string> performance_samples_uri() const;

    /** Same derivation again, swapping the trailing "/events" for "/custom_metrics" and "/infrastructure_metrics". */
    std::optional<std::string> custom_metrics_uri() const;
    std::optional<std::string> infrastructure_metrics_uri() const;

    /** Same derivation again, swapping the trailing "/events" for "/spans". */
    std::optional<std::string> spans_uri() const;

    bool is_enabled() const;

    void log(const std::string& message) const;
};

} // namespace forge_ops_tracker
