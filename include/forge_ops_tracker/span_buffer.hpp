#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/configuration.hpp"
#include "forge_ops_tracker/trace_parent.hpp"

namespace forge_ops_tracker {

/**
 * One trace's worth of spans (a request's, or a job's, own call tree), sharing a single trace id.
 * Held one-per-thread by forge_ops_tracker.cpp in a thread_local, the same isolation choice the
 * breadcrumb trail and set_user make, so only the owning thread ever touches it and no locking is
 * needed. Nesting comes from a stack of open span ids: a span opened while another is open becomes
 * its child, and anything else parents under the root.
 *
 * A trace is sent only when its root span took at least Configuration::trace_capture_threshold,
 * decided in finish() once the root ends, so a fast request costs nothing on the wire. Kinds are the
 * closed set the ingestion API accepts (controller, service, database, redis, http, job, other):
 * anything else is sent as "other", since one bad kind would make the server reject the whole trace.
 *
 * `incoming` comes from an incoming traceparent header when this trace continues another service's
 * (see trace_parent.hpp): the trace keeps that trace id, and the root span points at the caller's
 * span, which the server nests it under even though it arrives in a different upload. Whether the
 * spans are sent at all is Configuration::track_tracing as it was when the buffer was created: with
 * it off the buffer still exists, for its trace id, but finish() always returns nullopt.
 */
class SpanBuffer {
public:
    static constexpr std::size_t max_spans = 500;

    explicit SpanBuffer(const Configuration& configuration, std::optional<trace_parent::Context> incoming = std::nullopt);

    /** This trace's id: 32 lowercase hex characters. */
    const std::string& trace_id() const { return trace_id_; }

    /** Opens a span and returns its id; pair with close(). */
    std::string open();

    /** Closes a span opened with open() and records it. */
    void close(const std::string& id, const std::string& name, const std::string& kind, std::chrono::system_clock::time_point started_at, double duration_ms, const nlohmann::json& data);

    /** Records an already-finished span as a child of whatever is currently open. */
    void record_leaf(const std::string& name, const std::string& kind, std::chrono::system_clock::time_point started_at, double duration_ms, const nlohmann::json& data);

    /** The wire payload once the root has ended, or nullopt when it was faster than the threshold or track_tracing was off. */
    std::optional<nlohmann::json> finish(const std::string& root_name, std::chrono::system_clock::time_point started_at, double duration_ms) const;

    std::size_t span_count() const { return spans_.size(); }

    /**
     * A span's data as it goes on the wire: on a "database" span, a string "db.statement" is masked
     * with sql_statement::mask (every string and number literal becomes "?", cut at 4000 characters)
     * and a string "db.system" is lowercased, so raw SQL can never go out on a span; a blank one is
     * left out. Any other kind's data is returned unchanged.
     */
    static nlohmann::json span_data(const std::string& kind, const nlohmann::json& data);

private:
    const Configuration& configuration_;
    std::string trace_id_;
    std::string root_span_id_;
    std::optional<std::string> remote_parent_span_id_;
    bool send_;
    std::vector<nlohmann::json> spans_;
    std::vector<std::string> open_;

    std::string current_parent() const;
    void record(const std::string& id, const std::string& parent, const std::string& name, const std::string& kind, std::chrono::system_clock::time_point started_at, double duration_ms, const nlohmann::json& data);
    nlohmann::json build(const std::string& id, const std::optional<std::string>& parent, const std::string& name, const std::string& kind, std::chrono::system_clock::time_point started_at, double duration_ms, const nlohmann::json& data) const;
};

} // namespace forge_ops_tracker
