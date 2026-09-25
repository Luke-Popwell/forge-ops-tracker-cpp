#include "forge_ops_tracker/change.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>

namespace forge_ops_tracker {

namespace {

// ISO 8601 with milliseconds, UTC: the same format span_buffer.cpp sends.
std::string format_timestamp(std::chrono::system_clock::time_point time) {
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
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
    for (const char* known : change_kinds) {
        if (kind == known) {
            return known;
        }
    }
    return "other";
}

// The first max_chars UTF-8 characters of text, counting lead bytes only so the cut never lands
// inside a multibyte character.
std::string utf8_prefix(const std::string& text, std::size_t max_chars) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size(); i++) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (chars == max_chars) {
                return text.substr(0, i);
            }
            chars++;
        }
    }
    return text;
}

std::string trim(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }
    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }
    return text.substr(start, end - start);
}

} // namespace

std::optional<nlohmann::json> build_change(const Configuration& configuration, const std::string& kind, const std::string& title, const nlohmann::json& details, const ChangeOptions& options) {
    std::string trimmed = trim(title);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    nlohmann::json change = {
        {"kind", normalize_kind(kind)},
        {"title", utf8_prefix(trimmed, max_change_title_chars)},
        {"environment", options.environment.value_or(configuration.environment)},
        {"occurred_at", format_timestamp(options.occurred_at.value_or(std::chrono::system_clock::now()))},
    };
    if (details.is_object() && !details.empty()) {
        change["details"] = details;
    }
    if (options.service) {
        change["service"] = *options.service;
    }
    if (options.actor) {
        change["actor"] = *options.actor;
    }
    if (options.url) {
        change["url"] = *options.url;
    }
    if (options.id) {
        change["id"] = *options.id;
    }
    return change;
}

} // namespace forge_ops_tracker
