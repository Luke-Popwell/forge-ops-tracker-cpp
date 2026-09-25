#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/configuration.hpp"

namespace forge_ops_tracker {

/** The kinds of change ForgeOps accepts; record_change sends anything else as "other", since the server rejects an unknown kind outright. */
inline constexpr std::array<const char*, 6> change_kinds = {"feature_flag", "config", "migration", "dependency", "infrastructure", "other"};

/** The server's own limit on a change's title, in characters; a longer one is cut here rather than rejected there. */
inline constexpr std::size_t max_change_title_chars = 200;

/** The optional parts of a change, for record_change. Every field left unset is left out of the request. */
struct ChangeOptions {
    std::optional<std::string> environment; // nullopt means Configuration::environment
    std::optional<std::string> service;
    std::optional<std::string> actor;
    std::optional<std::string> url;
    std::optional<std::string> id; // your own idempotency key: a retried call with the same id records the change once
    std::optional<std::chrono::system_clock::time_point> occurred_at; // nullopt means now
};

/**
 * The request body for one change, or nullopt when the title is blank. `details` is sent only when it
 * is a non-empty JSON object. The title is trimmed and cut to max_change_title_chars characters, never
 * through the middle of a multibyte UTF-8 character. Internal: the public API is record_change in
 * forge_ops_tracker.hpp.
 */
std::optional<nlohmann::json> build_change(const Configuration& configuration, const std::string& kind, const std::string& title, const nlohmann::json& details, const ChangeOptions& options);

} // namespace forge_ops_tracker
