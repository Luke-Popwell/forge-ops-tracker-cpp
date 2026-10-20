#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace forge_ops_tracker {

/**
 * Strict privacy (Configuration::strict_privacy): for an app that can't let personal or health data
 * leave its own servers. With it on, only structured facts about an error go out (its class, its
 * backtrace frames, release, environment, server, trace id); free text, which is where personal
 * data hides (the message, context, tags, breadcrumbs, the SQL statement, the user's email and
 * username), never leaves the process. Ported from the server's own app/services/strict_privacy.rb,
 * which applies the same rules again on arrival when the project's own Strict privacy setting is on;
 * sdks/STRICT_PRIVACY.md is the spec every SDK follows, and sdks/strict_privacy_vectors.json pins
 * the exact results (see tests/fixtures).
 *
 * Grouping keeps working without the message: message_fingerprint is a one-way hash of the message
 * with its numbers and ids taken out, so two messages that differ only in ids hash the same.
 */
namespace strict_privacy {

/**
 * The message with every "0x" plus 4 or more hex digits replaced by "0x", then every UUID that
 * isn't directly next to an ASCII word character ([A-Za-z0-9_]) by "<uuid>", then every run of
 * ASCII digits that isn't directly next to one by "<n>". A byte scanner rather than std::regex:
 * "word character" is ASCII only here, so a UTF-8 byte of an accented letter or a digit in another
 * script is never one, whatever the regex engine's locale would say.
 */
std::string normalize_message(const std::string& message);

/** The first 32 characters of the lowercase hex SHA-256 of normalize_message(message)'s bytes. */
std::string message_fingerprint(const std::string& message);

/** Lowercase hex SHA-256 of `bytes`. Self-contained, so this library needs nothing beyond libcurl and nlohmann/json. */
std::string sha256_hex(const std::string& bytes);

/**
 * A request or page path as strict privacy sends it, with no query or fragment. A route template
 * (any segment that starts with ":" or "*", or is wrapped in {}, <> or []) is kept as it is:
 * "/patients/:id/notes" stays. A raw path keeps only its first segment (":id" when that holds an
 * ASCII digit or is longer than 40 characters), then "/:path" if there was more:
 * "/patients/jane-doe/12" is "/patients/:path", "/42/edit" is "/:id/:path". No segments is "/".
 */
std::string strict_path(const std::string& path);

/** The name strict_path had before its rule changed: the same function, kept so existing code still builds. */
std::string path_without_ids(const std::string& path);

/**
 * A span's name with no raw path beyond its first segment and no outbound address beyond its host,
 * read by hand rather than with a URL parser: a name starting with "/" goes through strict_path
 * ("/patients/42" is "/patients/:path"); so does a method followed by a path ("GET /patients/42/notes"
 * is "GET /patients/:path", "GET /patients/:id" stays); a
 * method followed by an address keeps only the host ("GET https://api.example.com/patients/42?x=1"
 * is "GET api.example.com"), which needs a scheme, a port, a dot or "localhost" to count as one, or
 * the whole target gets the path rule instead. Any other name ("OrdersController#create",
 * "patients#show") is returned unchanged.
 */
std::string span_name(const std::string& name);

/**
 * Which request something happened in: a route template or a controller action is kept, a raw
 * path keeps only its first segment, by span_name's rule. A blank name is returned unchanged.
 */
std::string transaction_name(const std::string& name);

/**
 * An error event's payload reduced to what strict privacy sends: message "" plus
 * message_fingerprint (computed from the payload's message as it stands, so after PII scrubbing
 * when that's on), context and tags cut to `allowed_fields` with plain values only (left out when
 * nothing is left), user cut to its id as a string (left out without one), and no breadcrumbs,
 * endpoint, sql_statement or source context on any backtrace frame.
 */
nlohmann::json event(nlohmann::json payload, const std::vector<std::string>& allowed_fields);

/** A span's data cut to "db.statement" (already masked) and "db.system"; an object either way. */
nlohmann::json span_data(const nlohmann::json& data);

} // namespace strict_privacy
} // namespace forge_ops_tracker
