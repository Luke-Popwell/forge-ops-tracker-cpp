#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/configuration.hpp"

namespace forge_ops_tracker {

/**
 * Delivers one payload over HTTP. Every failure mode: DNS, connection, timeout, a non-2xx
 * response: is caught here and turned into a `false` return rather than a thrown exception,
 * since a broken or unreachable tracker must never be able to break the host app. Uses libcurl:
 * not a bespoke choice for this SDK specifically, but the closest C++ equivalent to "the
 * platform's own standard HTTP client" every other SDK in this repo reaches for (Ruby's
 * Net::HTTP, Python's urllib, PHP's curl extension already wrapping the same library under the
 * hood): there's no HTTP client in the C++ standard library itself, and libcurl is close to
 * universally available wherever a C++ toolchain already is.
 */
class Client {
public:
    explicit Client(const Configuration& configuration);

    bool deliver(const nlohmann::json& payload) const;

    /**
     * Same delivery contract as deliver(), against the DSN's performance_samples endpoint (see
     * Configuration::performance_samples_uri). `samples` is wrapped as {"samples": [...]}, the shape
     * Api::V1::PerformanceSamplesController expects.
     */
    bool deliver_performance_samples(const nlohmann::json& samples) const;

    /**
     * Same delivery contract again, against the DSN's custom metrics and infrastructure metrics
     * endpoints. `entries` is wrapped as {"metrics": [...]}, the shape Api::V1::CustomMetricsController
     * and Api::V1::InfrastructureMetricsController expect.
     */
    bool deliver_metrics(const nlohmann::json& entries) const;
    bool deliver_infrastructure_metrics(const nlohmann::json& entries) const;

    /**
     * Same delivery contract again, against the DSN's spans endpoint (see Configuration::spans_uri).
     * `trace` is sent as-is: {"trace_id": ..., "spans": [...]}, the shape Api::V1::SpansController
     * expects.
     */
    bool deliver_spans(const nlohmann::json& trace) const;

    /**
     * Same delivery contract again, against the DSN's changes endpoint (see Configuration::changes_uri).
     * `change` is sent as-is, the one change record_change built. A 403 (a plan without change
     * tracking) is an ordinary non-2xx here: a quiet `false`.
     */
    bool deliver_change(const nlohmann::json& change) const;

private:
    const Configuration& configuration_;

    bool post(const std::optional<std::string>& uri, const nlohmann::json& payload) const;
};

} // namespace forge_ops_tracker
