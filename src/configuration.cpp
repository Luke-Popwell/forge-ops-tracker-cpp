#include "forge_ops_tracker/configuration.hpp"

#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace forge_ops_tracker {

namespace {

// scheme://[userinfo@]host[:port][/path]: matches the same shape every other SDK's own DSN
// parser in this repo handles, via std::regex (standard library, no extra dependency) rather than
// a general-purpose URI parser: a DSN's shape is simple and fixed enough that this covers it
// completely.
//
// Heap-allocated and never freed, on purpose: a function-local static is destroyed at exit in
// reverse order of construction, and this one is built on the first DSN parse, after the globals
// that own the delivery threads, so it used to be destroyed before them. A delivery still running
// at exit then matched against a destroyed regex (a crash at the end of an ordinary `return 0`).
// Leaking it keeps it valid for as long as any thread could still call it.
const std::regex& dsn_pattern() {
    static const std::regex* const pattern = new std::regex(R"(^(https?)://(?:([^:@/]*)@)?([^/]+)(/.*)?$)");
    return *pattern;
}

// The trailing segment every derived endpoint replaces. A plain literal, not a static std::string,
// for the same reason dsn_pattern() leaks: nothing here may be destroyed while a thread can read it.
constexpr const char kEventsSuffix[] = "/events";

bool ends_with_events(const std::string& uri) {
    const std::size_t length = sizeof(kEventsSuffix) - 1;
    return uri.size() >= length && uri.compare(uri.size() - length, length, kEventsSuffix) == 0;
}

std::string without_events_suffix(const std::string& uri) {
    return uri.substr(0, uri.size() - (sizeof(kEventsSuffix) - 1));
}

std::string percent_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            std::istringstream hex_stream(in.substr(i + 1, 2));
            int value = 0;
            if (hex_stream >> std::hex >> value) {
                out += static_cast<char>(value);
                i += 2;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

struct ParsedDsn {
    std::string scheme;
    std::optional<std::string> api_key;
    std::string ingestion_uri;
};

std::optional<ParsedDsn> parse(const std::optional<std::string>& dsn) {
    if (!dsn || dsn->empty()) {
        return std::nullopt;
    }
    std::smatch match;
    if (!std::regex_match(*dsn, match, dsn_pattern())) {
        return std::nullopt;
    }

    ParsedDsn parsed;
    parsed.scheme = match[1].str();
    std::string userinfo = match[2].str();
    if (!userinfo.empty()) {
        parsed.api_key = percent_decode(userinfo);
    }
    std::string path = match[4].matched ? match[4].str() : "";
    parsed.ingestion_uri = parsed.scheme + "://" + match[3].str() + path;
    return parsed;
}

} // namespace

std::optional<std::string> Configuration::api_key() const {
    auto parsed = parse(dsn);
    if (!parsed || !parsed->api_key) {
        return std::nullopt;
    }
    return parsed->api_key;
}

std::optional<std::string> Configuration::ingestion_uri() const {
    auto parsed = parse(dsn);
    if (!parsed) {
        return std::nullopt;
    }
    return parsed->ingestion_uri;
}

std::optional<std::string> Configuration::performance_samples_uri() const {
    auto uri = ingestion_uri();
    if (!uri) {
        return std::nullopt;
    }
    if (ends_with_events(*uri)) {
        return without_events_suffix(*uri) + "/performance_samples";
    }
    return uri;
}

namespace {
std::optional<std::string> swap_events_suffix(const std::optional<std::string>& uri, const std::string& replacement) {
    if (!uri) {
        return std::nullopt;
    }
    if (ends_with_events(*uri)) {
        return without_events_suffix(*uri) + replacement;
    }
    return uri;
}
} // namespace

std::optional<std::string> Configuration::custom_metrics_uri() const {
    return swap_events_suffix(ingestion_uri(), "/custom_metrics");
}

std::optional<std::string> Configuration::infrastructure_metrics_uri() const {
    return swap_events_suffix(ingestion_uri(), "/infrastructure_metrics");
}

std::optional<std::string> Configuration::changes_uri() const {
    return swap_events_suffix(ingestion_uri(), "/changes");
}

std::optional<std::string> Configuration::spans_uri() const {
    auto uri = ingestion_uri();
    if (!uri) {
        return std::nullopt;
    }
    if (ends_with_events(*uri)) {
        return without_events_suffix(*uri) + "/spans";
    }
    return uri;
}

bool Configuration::is_enabled() const {
    if (!dsn || dsn->empty()) {
        return false;
    }
    if (!api_key()) {
        return false;
    }
    return enabled_environments.count(environment) > 0;
}

std::string Configuration::default_environment() {
    const char* value = std::getenv("FORGE_OPS_ENVIRONMENT");
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string("production");
}

void Configuration::log(const std::string& message) const {
    if (logger) {
        logger(message);
    }
}

bool Configuration::should_propagate_trace(const std::optional<std::string>& host) const {
    if (!propagate_traces) {
        return false;
    }
    if (!trace_propagation_targets) {
        return true;
    }
    std::string normalized = host.value_or("");
    for (char& c : normalized) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (normalized.empty()) {
        return false;
    }
    for (const auto& target : *trace_propagation_targets) {
        if (const auto* pattern = std::get_if<std::regex>(&target)) {
            if (std::regex_search(normalized, *pattern)) {
                return true;
            }
            continue;
        }
        std::string domain = std::get<std::string>(target);
        for (char& c : domain) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (!domain.empty() && domain[0] == '.') {
            domain.erase(0, 1);
        }
        if (domain.empty() || normalized.size() < domain.size()) {
            continue;
        }
        if (normalized == domain) {
            return true;
        }
        // Same length but not equal leaves no room for the "." a subdomain needs.
        std::size_t prefix = normalized.size() - domain.size();
        if (prefix > 0 && normalized[prefix - 1] == '.' && normalized.compare(prefix, std::string::npos, domain) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace forge_ops_tracker
