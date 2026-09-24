#include "forge_ops_tracker/span_buffer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

namespace forge_ops_tracker {

namespace {

// ISO 8601 with milliseconds, UTC. gmtime_r, not std::gmtime, for the same shared-buffer reason
// add_breadcrumb documents.
std::string format_timestamp(std::chrono::system_clock::time_point time) {
    auto since_epoch = time.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
    std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    long remainder = static_cast<long>(millis % 1000);
    if (remainder < 0) {
        remainder = 0;
    }
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char whole[32];
    std::strftime(whole, sizeof(whole), "%Y-%m-%dT%H:%M:%S", &utc);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03ldZ", whole, remainder);
    return out;
}

const char* normalize_kind(const std::string& kind) {
    static const char* const kinds[] = {"controller", "service", "database", "redis", "http", "job", "other"};
    for (const char* known : kinds) {
        if (kind == known) {
            return known;
        }
    }
    return "other";
}

} // namespace

SpanBuffer::SpanBuffer(const Configuration& configuration, std::optional<trace_parent::Context> incoming)
    : configuration_(configuration),
      trace_id_(incoming ? incoming->trace_id : trace_parent::generate_trace_id()),
      root_span_id_(trace_parent::generate_span_id()),
      remote_parent_span_id_(incoming ? std::optional<std::string>(incoming->parent_span_id) : std::nullopt),
      send_(configuration.track_tracing) {}

std::string SpanBuffer::current_parent() const {
    return open_.empty() ? root_span_id_ : open_.back();
}

std::string SpanBuffer::open() {
    std::string id = trace_parent::generate_span_id();
    open_.push_back(id);
    return id;
}

void SpanBuffer::close(const std::string& id, const std::string& name, const std::string& kind, std::chrono::system_clock::time_point started_at, double duration_ms, const nlohmann::json& data) {
    // The parent is whatever was open just beneath this span when it started, not whatever is open
    // now: spans close in strict reverse order of opening, so it is the entry below this one.
    auto it = std::find(open_.rbegin(), open_.rend(), id);
    std::string parent = root_span_id_;
    if (it != open_.rend()) {
        auto below = std::next(it);
        if (below != open_.rend()) {
            parent = *below;
        }
        open_.erase(std::next(it).base());
    }
    record(id, parent, name, kind, started_at, duration_ms, data);
}

void SpanBuffer::record_leaf(const std::string& name, const std::string& kind, std::chrono::system_clock::time_point started_at, double duration_ms, const nlohmann::json& data) {
    record(trace_parent::generate_span_id(), current_parent(), name, kind, started_at, duration_ms, data);
}

void SpanBuffer::record(const std::string& id, const std::string& parent, const std::string& name, const std::string& kind, std::chrono::system_clock::time_point started_at, double duration_ms, const nlohmann::json& data) {
    if (spans_.size() >= max_spans - 1) {
        return; // leave room for the root
    }
    spans_.push_back(build(id, parent, name, kind, started_at, duration_ms, data));
}

nlohmann::json SpanBuffer::build(const std::string& id, const std::optional<std::string>& parent, const std::string& name, const std::string& kind, std::chrono::system_clock::time_point started_at, double duration_ms, const nlohmann::json& data) const {
    nlohmann::json span = {
        {"span_id", id},
        {"parent_span_id", parent ? nlohmann::json(*parent) : nlohmann::json(nullptr)},
        {"name", name},
        {"kind", normalize_kind(kind)},
        {"started_at", format_timestamp(started_at)},
        {"duration_ms", std::round(duration_ms * 100.0) / 100.0},
        {"environment", configuration_.environment},
        {"release", configuration_.release ? nlohmann::json(*configuration_.release) : nlohmann::json(nullptr)},
        {"data", data.is_object() ? data : nlohmann::json::object()},
    };
    return span;
}

std::optional<nlohmann::json> SpanBuffer::finish(const std::string& root_name, std::chrono::system_clock::time_point started_at, double duration_ms) const {
    if (!send_ || duration_ms < static_cast<double>(configuration_.trace_capture_threshold.count())) {
        return std::nullopt;
    }

    nlohmann::json spans = nlohmann::json::array();
    spans.push_back(build(root_span_id_, remote_parent_span_id_, root_name, "controller", started_at, duration_ms, nlohmann::json::object()));
    for (const auto& span : spans_) {
        spans.push_back(span);
    }
    return nlohmann::json{{"trace_id", trace_id_}, {"spans", spans}};
}

} // namespace forge_ops_tracker
