#pragma once

#include <optional>
#include <string>

namespace forge_ops_tracker {
namespace trace_parent {

/**
 * Reads and writes the W3C Trace Context `traceparent` header (https://www.w3.org/TR/trace-context/),
 * the vendor-neutral format for carrying one trace across service boundaries:
 * `00-<32 hex trace id>-<16 hex parent span id>-<2 hex flags>`. Mirrors gems/forge_ops_tracker's own
 * TraceParent: parse() lets ScopedTrace continue a caller's trace, and build() is what ScopedHttpSpan
 * hands over for the next service along.
 *
 * Strict on the way in, the posture the spec asks receivers to take: a malformed value, uppercase hex,
 * the reserved version `ff`, or an all-zero trace or parent id all mean "no usable header" (parse()
 * returns nullopt and a fresh trace starts), never half-trusted. A future version is still accepted when
 * its first four fields have version 00's shape; version 00 itself must have exactly four.
 */
inline constexpr const char* header = "traceparent";

/** A usable incoming header's two ids: the caller's trace, and the caller's span that this work runs under. */
struct Context {
    std::string trace_id;
    std::string parent_span_id;
};

std::optional<Context> parse(const std::string& value);

std::string build(const std::string& trace_id, const std::string& span_id);

/** 32 lowercase hex characters, never all zeros (the spec's one invalid value). */
std::string generate_trace_id();

/** 16 lowercase hex characters, never all zeros. */
std::string generate_span_id();

/**
 * The host of an absolute URL (`scheme://[userinfo@]host[:port]/...`), lowercased, or nullopt when
 * there isn't one. An IPv6 literal keeps its brackets.
 */
std::optional<std::string> url_host(const std::string& url);

} // namespace trace_parent
} // namespace forge_ops_tracker
