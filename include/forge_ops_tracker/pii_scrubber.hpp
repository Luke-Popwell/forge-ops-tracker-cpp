#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace forge_ops_tracker {

/**
 * Redacts likely-sensitive content out of a payload before it ever leaves this process -- the
 * same patterns ForgeOps itself applies again on arrival (defense in depth: this layer keeps the
 * data off the wire; the server-side layer is what actually protects the database). Ported from
 * app/services/pii_scrubber.rb -- same key list, same 8 regex patterns, same "[LABEL FILTERED]"
 * replacement format, same REDACTED constant. Deliberately does NOT support
 * Project#additional_sensitive_keys -- confirmed server-side only (see that file's own header
 * comment: extending the pattern list to arbitrary customer regexes is a ReDoS risk best kept out
 * of every client).
 */
namespace pii_scrubber {

extern const std::string kRedacted; // "[FILTERED]"

/**
 * value may be any JSON type (object, array, string, number, bool, null); key is the enclosing
 * object key `value` was found under (nullopt for a bare top-level value or an array element),
 * and is what the key-name check runs against.
 */
nlohmann::json scrub(const nlohmann::json& value, const std::optional<std::string>& key = std::nullopt);

std::string scrub_string(const std::string& text);

} // namespace pii_scrubber
} // namespace forge_ops_tracker
