#pragma once

#include <functional>
#include <optional>
#include <set>
#include <string>

namespace forge_ops_tracker {

/**
 * Holds a single ForgeOps DSN plus everything else the client needs to build and deliver events.
 * Mirrors gems/forge_ops_tracker/lib/forge_ops_tracker/configuration.rb -- a single DSN string
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
     * root. Defaults empty (meaning: never in_app) -- set explicitly to your app's own source
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
     * this repo -- see EventBuilder's own header comment for why this specific client's capture
     * function is a documented no-op regardless of this value: backtrace_symbols() never produces
     * a real file+line pair to read in the first place.
     */
    bool capture_source_context = true;

    std::function<void(const std::string&)> logger;

    std::optional<std::string> api_key() const;

    /** The ingestion URL with credentials stripped out (they travel as the Authorization header instead). */
    std::optional<std::string> ingestion_uri() const;

    bool is_enabled() const;

    void log(const std::string& message) const;
};

} // namespace forge_ops_tracker
