#include "forge_ops_tracker/pii_scrubber.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>
#include <vector>

namespace forge_ops_tracker {
namespace pii_scrubber {

const std::string kRedacted = "[FILTERED]";

namespace {

const std::vector<std::string>& sensitive_keys() {
    static const std::vector<std::string> keys = {
        "password", "passwd", "pwd",
        "secret", "apisecret", "clientsecret", "secretkey",
        "token", "accesstoken", "refreshtoken", "apikey", "apitoken", "authorization", "authtoken", "bearer", "sessiontoken", "csrftoken",
        "creditcard", "cardnumber", "cardnum", "cvv", "cvv2", "cvc",
        "ssn", "socialsecuritynumber", "socialsecurity",
        "privatekey",
    };
    return keys;
}

struct Pattern {
    std::string label;
    std::regex regex;
};

// std::regex's default ECMAScript grammar accepts every one of these 8 patterns unmodified from
// app/services/pii_scrubber.rb's own Ruby syntax -- verified directly against real matching
// input for each (see test_pii_scrubber.cpp), not assumed to translate cleanly just because the
// syntax looks the same.
const std::vector<Pattern>& patterns() {
    static const std::vector<Pattern> built = [] {
        std::vector<Pattern> p;
        p.push_back({"EMAIL", std::regex(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})")});
        p.push_back({"SSN", std::regex(R"(\b\d{3}-\d{2}-\d{4}\b)")});
        p.push_back({"CREDIT CARD", std::regex(R"(\b\d{4}[ -]\d{4}[ -]\d{4}[ -]\d{1,4}\b)")});
        p.push_back({"BEARER TOKEN", std::regex(R"(\bBearer\s+[A-Za-z0-9\-._~+/]+=*)", std::regex::icase)});
        p.push_back({"JWT", std::regex(R"(\bey[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b)")});
        p.push_back({"AWS KEY", std::regex(R"(\bAKIA[0-9A-Z]{16}\b)")});
        p.push_back({"STRIPE KEY", std::regex(R"(\b[sr]k_(?:live|test)_[A-Za-z0-9]{10,}\b)")});
        p.push_back({"GITHUB TOKEN", std::regex(R"(\bgh[pousr]_[A-Za-z0-9]{20,}\b)")});
        return p;
    }();
    return built;
}

bool is_sensitive_key(const std::optional<std::string>& key) {
    if (!key || key->empty()) {
        return false;
    }
    std::string normalized;
    normalized.reserve(key->size());
    for (char c : *key) {
        char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::isalnum(static_cast<unsigned char>(lower))) {
            normalized += lower;
        }
    }
    for (const auto& sensitive : sensitive_keys()) {
        if (normalized.find(sensitive) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string scrub_string(const std::string& text) {
    std::string result = text;
    for (const auto& pattern : patterns()) {
        result = std::regex_replace(result, pattern.regex, "[" + pattern.label + " FILTERED]");
    }
    return result;
}

nlohmann::json scrub(const nlohmann::json& value, const std::optional<std::string>& key) {
    if (is_sensitive_key(key) && !value.is_null()) {
        return kRedacted;
    }

    if (value.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            out[it.key()] = scrub(it.value(), it.key());
        }
        return out;
    }

    if (value.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& element : value) {
            out.push_back(scrub(element, key));
        }
        return out;
    }

    if (value.is_string()) {
        return scrub_string(value.get<std::string>());
    }

    return value;
}

} // namespace pii_scrubber
} // namespace forge_ops_tracker
