#include "forge_ops_tracker/trace_parent.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>

namespace forge_ops_tracker {
namespace trace_parent {

namespace {

// "vv-" + 32 + "-" + 16 + "-ff": the four fields every version shares.
constexpr std::size_t fixed_length = 55;

bool is_lower_hex(const std::string& value, std::size_t from, std::size_t length) {
    for (std::size_t i = from; i < from + length; i++) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool all_zeros(const std::string& id) {
    return id.find_first_not_of('0') == std::string::npos;
}

std::string random_hex(std::size_t bytes) {
    // random_device can throw where no entropy source exists; an id only has to be unique within
    // one project's traces, not unpredictable, so fall back to a clock-seeded engine.
    std::mt19937_64 fallback(static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::random_device device;
    std::string out;
    out.reserve(bytes * 2);
    char buffer[3];
    for (std::size_t i = 0; i < bytes; i++) {
        unsigned value;
        try {
            value = device() & 0xff;
        } catch (...) {
            value = static_cast<unsigned>(fallback() & 0xff);
        }
        std::snprintf(buffer, sizeof(buffer), "%02x", value);
        out += buffer;
    }
    return out;
}

std::string random_non_zero_hex(std::size_t bytes) {
    std::string id;
    do {
        id = random_hex(bytes);
    } while (all_zeros(id));
    return id;
}

} // namespace

std::optional<Context> parse(const std::string& raw) {
    auto first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::nullopt;
    }
    auto last = raw.find_last_not_of(" \t\r\n");
    std::string value = raw.substr(first, last - first + 1);

    if (value.size() < fixed_length) {
        return std::nullopt;
    }
    if (!is_lower_hex(value, 0, 2) || value[2] != '-' || !is_lower_hex(value, 3, 32) || value[35] != '-' ||
        !is_lower_hex(value, 36, 16) || value[52] != '-' || !is_lower_hex(value, 53, 2)) {
        return std::nullopt;
    }
    std::string version = value.substr(0, 2);
    if (version == "ff") {
        return std::nullopt;
    }
    if (value.size() > fixed_length && (version == "00" || value[fixed_length] != '-')) {
        return std::nullopt;
    }
    Context context{value.substr(3, 32), value.substr(36, 16)};
    if (all_zeros(context.trace_id) || all_zeros(context.parent_span_id)) {
        return std::nullopt;
    }
    return context;
}

std::string build(const std::string& trace_id, const std::string& span_id) {
    // Always "01" (sampled) on the way out: whether a trace is sent is only decided once it's over
    // (see Configuration::trace_capture_threshold), long after this header has gone out, so "this
    // may be recorded" is the only honest answer. The next service makes its own decision either way.
    return "00-" + trace_id + "-" + span_id + "-01";
}

std::string generate_trace_id() {
    return random_non_zero_hex(16);
}

std::string generate_span_id() {
    return random_non_zero_hex(8);
}

std::optional<std::string> url_host(const std::string& url) {
    auto separator = url.find("://");
    if (separator == std::string::npos || separator == 0) {
        return std::nullopt;
    }
    std::size_t start = separator + 3;
    std::size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    auto at = authority.rfind('@');
    std::string host_and_port = at == std::string::npos ? authority : authority.substr(at + 1);

    std::string host;
    if (!host_and_port.empty() && host_and_port[0] == '[') {
        auto close = host_and_port.find(']');
        if (close == std::string::npos) {
            return std::nullopt;
        }
        host = host_and_port.substr(0, close + 1);
    } else {
        host = host_and_port.substr(0, host_and_port.find(':'));
    }
    if (host.empty()) {
        return std::nullopt;
    }
    for (char& c : host) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return host;
}

} // namespace trace_parent
} // namespace forge_ops_tracker
