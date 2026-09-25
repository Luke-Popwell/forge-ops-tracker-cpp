#include "forge_ops_tracker/configuration.hpp"

#include <cctype>
#include <regex>
#include <sstream>

namespace forge_ops_tracker {

namespace {

// scheme://[userinfo@]host[:port][/path]: matches the same shape every other SDK's own DSN
// parser in this repo handles, via std::regex (standard library, no extra dependency) rather than
// a general-purpose URI parser: a DSN's shape is simple and fixed enough that this covers it
// completely.
const std::regex& dsn_pattern() {
    static const std::regex pattern(R"(^(https?)://(?:([^:@/]*)@)?([^/]+)(/.*)?$)");
    return pattern;
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
    static const std::string suffix = "/events";
    if (uri->size() >= suffix.size() && uri->compare(uri->size() - suffix.size(), suffix.size(), suffix) == 0) {
        return uri->substr(0, uri->size() - suffix.size()) + "/performance_samples";
    }
    return uri;
}

namespace {
std::optional<std::string> swap_events_suffix(const std::optional<std::string>& uri, const std::string& replacement) {
    if (!uri) {
        return std::nullopt;
    }
    static const std::string suffix = "/events";
    if (uri->size() >= suffix.size() && uri->compare(uri->size() - suffix.size(), suffix.size(), suffix) == 0) {
        return uri->substr(0, uri->size() - suffix.size()) + replacement;
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
    static const std::string suffix = "/events";
    if (uri->size() >= suffix.size() && uri->compare(uri->size() - suffix.size(), suffix.size(), suffix) == 0) {
        return uri->substr(0, uri->size() - suffix.size()) + "/spans";
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
