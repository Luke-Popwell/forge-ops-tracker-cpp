#include "forge_ops_tracker/strict_privacy.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>

namespace forge_ops_tracker {
namespace strict_privacy {

namespace {

constexpr std::size_t kFingerprintLength = 32;
constexpr std::size_t kMaxSegmentLength = 40;
constexpr std::size_t kUuidLength = 36;

// Plain ASCII checks on the raw byte, never the <cctype> ones: those depend on the C locale, and a
// UTF-8 byte at or above 0x80 must never count as a digit, letter or word character.
bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_hex(char c) {
    return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_word(char c) {
    return is_ascii_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// The same characters Ruby's \s matches.
bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string replace_hex_object_ids(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        if (in[i] == '0' && i + 1 < in.size() && in[i + 1] == 'x') {
            std::size_t end = i + 2;
            while (end < in.size() && is_hex(in[end])) {
                end++;
            }
            if (end - (i + 2) >= 4) {
                out += "0x";
                i = end;
                continue;
            }
        }
        out += in[i];
        i++;
    }
    return out;
}

// 8-4-4-4-12 hex digits starting at `at`, either case.
bool is_uuid_at(const std::string& in, std::size_t at) {
    if (at + kUuidLength > in.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset < kUuidLength; offset++) {
        char c = in[at + offset];
        bool dash = offset == 8 || offset == 13 || offset == 18 || offset == 23;
        if (dash ? c != '-' : !is_hex(c)) {
            return false;
        }
    }
    return true;
}

std::string replace_uuids(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        bool free_before = i == 0 || !is_word(in[i - 1]);
        std::size_t end = i + kUuidLength;
        if (free_before && is_uuid_at(in, i) && (end == in.size() || !is_word(in[end]))) {
            out += "<uuid>";
            i = end;
            continue;
        }
        out += in[i];
        i++;
    }
    return out;
}

// A digit run only counts when it starts and ends away from a word character. A shorter run from
// the same start can never qualify (its next character is a digit), and a start inside a run fails
// on the digit before it, so checking the longest run from each free start is the whole rule.
std::string replace_numbers(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        if (is_ascii_digit(in[i]) && (i == 0 || !is_word(in[i - 1]))) {
            std::size_t end = i;
            while (end < in.size() && is_ascii_digit(in[end])) {
                end++;
            }
            if (end == in.size() || !is_word(in[end])) {
                out += "<n>";
                i = end;
                continue;
            }
        }
        out += in[i];
        i++;
    }
    return out;
}

// Characters, not bytes, the way Ruby's String#length counts them: every byte that isn't a UTF-8
// continuation byte starts one.
std::size_t character_count(const std::string& value) {
    return static_cast<std::size_t>(std::count_if(value.begin(), value.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// The length of a leading "scheme://" (a letter, then letters, digits, "+", "." or "-"), any case
// and any scheme, or 0 when there isn't one.
std::size_t scheme_length(const std::string& target) {
    auto is_letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (target.empty() || !is_letter(target[0])) {
        return 0;
    }
    std::size_t i = 1;
    while (i < target.size() && (is_letter(target[i]) || is_ascii_digit(target[i]) || target[i] == '+' || target[i] == '.' || target[i] == '-')) {
        i++;
    }
    return target.compare(i, 3, "://") == 0 ? i + 3 : 0;
}

// An outbound call's host, read by hand rather than with a URL parser (one that rejects an address
// must never cause the whole address to be sent): whatever comes before the first "/", "?" or "#",
// without a scheme, credentials or port, case kept. It's a host when the address had a scheme or a
// port, or it has a dot or is "localhost"; otherwise (a relative path) an empty string.
std::string host_of(const std::string& target) {
    std::size_t scheme = scheme_length(target);
    std::string authority = target.substr(scheme);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    auto at = authority.rfind('@'); // the last one: a password can hold an "@", a host never does
    if (at != std::string::npos) {
        authority.erase(0, at + 1);
    }

    std::size_t digits = 0;
    while (digits < authority.size() && is_ascii_digit(authority[authority.size() - 1 - digits])) {
        digits++;
    }
    bool port = digits > 0 && digits < authority.size() && authority[authority.size() - 1 - digits] == ':';
    std::string host = port ? authority.substr(0, authority.size() - 1 - digits) : authority;
    if (host.empty()) {
        return "";
    }

    std::string lowered = host;
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    bool is_host = scheme > 0 || port || host.find('.') != std::string::npos || lowered == "localhost";
    return is_host ? host : "";
}

bool starts_with_ignoring_case(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); i++) {
        if (std::toupper(static_cast<unsigned char>(value[i])) != static_cast<unsigned char>(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool is_blank(const std::string& value) {
    return std::all_of(value.begin(), value.end(), is_space);
}

nlohmann::json allowed(const nlohmann::json& fields, const std::vector<std::string>& allowed_fields) {
    nlohmann::json kept = nlohmann::json::object();
    if (!fields.is_object()) {
        return kept;
    }
    for (const auto& name : allowed_fields) {
        auto value = fields.find(name);
        if (value != fields.end() && (value->is_string() || value->is_number() || value->is_boolean())) {
            kept[name] = *value;
        }
    }
    return kept;
}

// Rotations and the round constants of FIPS 180-4's SHA-256.
std::uint32_t rotate_right(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

} // namespace

std::string sha256_hex(const std::string& bytes) {
    std::uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    // The message, a 1 bit, zeros up to 56 bytes mod 64, then its length in bits as 64 bits big endian.
    std::string padded = bytes;
    padded += static_cast<char>(0x80);
    while (padded.size() % 64 != 56) {
        padded += static_cast<char>(0x00);
    }
    std::uint64_t bit_length = static_cast<std::uint64_t>(bytes.size()) * 8;
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded += static_cast<char>((bit_length >> shift) & 0xff);
    }

    for (std::size_t block = 0; block < padded.size(); block += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            const auto* p = reinterpret_cast<const unsigned char*>(padded.data() + block + i * 4);
            w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
        }
        for (int i = 16; i < 64; i++) {
            std::uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; i++) {
            std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            std::uint32_t choose = (e & f) ^ (~e & g);
            std::uint32_t temp1 = h + s1 + choose + kRoundConstants[i] + w[i];
            std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::string out;
    out.reserve(64);
    char buffer[9];
    for (std::uint32_t word : state) {
        std::snprintf(buffer, sizeof(buffer), "%08x", word);
        out += buffer;
    }
    return out;
}

std::string normalize_message(const std::string& message) {
    return replace_numbers(replace_uuids(replace_hex_object_ids(message)));
}

std::string message_fingerprint(const std::string& message) {
    return sha256_hex(normalize_message(message)).substr(0, kFingerprintLength);
}

std::string path_without_ids(const std::string& path) {
    std::string without_query = path.substr(0, path.find_first_of("?#"));

    std::vector<std::string> segments;
    std::size_t start = 0;
    while (true) {
        std::size_t slash = without_query.find('/', start);
        segments.push_back(without_query.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    // Ruby's String#split drops trailing empty segments, so "/" and "/a/" come out as "" and "/a".
    while (!segments.empty() && segments.back().empty()) {
        segments.pop_back();
    }

    std::string joined;
    for (std::size_t i = 0; i < segments.size(); i++) {
        const std::string& segment = segments[i];
        bool is_id = std::any_of(segment.begin(), segment.end(), is_ascii_digit) || character_count(segment) > kMaxSegmentLength;
        joined += (i == 0 ? "" : "/") + (is_id ? std::string(":id") : segment);
    }
    return joined.empty() ? "/" : joined;
}

std::string span_name(const std::string& name) {
    if (!name.empty() && name[0] == '/') {
        return path_without_ids(name);
    }
    static const char* const methods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
    for (const char* method : methods) {
        std::string upper(method);
        if (!starts_with_ignoring_case(name, upper)) {
            continue;
        }
        std::size_t target_start = upper.size();
        while (target_start < name.size() && is_space(name[target_start])) {
            target_start++;
        }
        if (target_start == upper.size() || target_start == name.size()) {
            continue;
        }
        std::size_t target_end = target_start;
        while (target_end < name.size() && !is_space(name[target_end])) {
            target_end++;
        }
        std::string target = name.substr(target_start, target_end - target_start);

        if (target[0] == '/') {
            return upper + " " + path_without_ids(target);
        }
        std::string host = host_of(target);
        return upper + " " + (host.empty() ? path_without_ids(target) : host);
    }
    return name;
}

std::string transaction_name(const std::string& name) {
    return is_blank(name) ? name : span_name(name);
}

nlohmann::json event(nlohmann::json payload, const std::vector<std::string>& allowed_fields) {
    auto message = payload.find("message");
    std::string text = message != payload.end() && message->is_string() ? message->get<std::string>() : std::string();
    payload["message"] = "";
    payload["message_fingerprint"] = message_fingerprint(text);

    payload.erase("breadcrumbs");
    payload.erase("endpoint");
    payload.erase("sql_statement");

    for (const char* key : {"context", "tags"}) {
        nlohmann::json kept = payload.contains(key) ? allowed(payload[key], allowed_fields) : nlohmann::json::object();
        if (kept.empty()) {
            payload.erase(key);
        } else {
            payload[key] = kept;
        }
    }

    // The id only, always a string; never email or username, and no user at all without an id.
    std::string user_id;
    if (payload.contains("user") && payload["user"].is_object()) {
        auto id = payload["user"].find("id");
        if (id != payload["user"].end()) {
            if (id->is_string() && !is_blank(id->get<std::string>())) {
                user_id = id->get<std::string>();
            } else if (id->is_number()) {
                user_id = id->dump();
            }
        }
    }
    if (user_id.empty()) {
        payload.erase("user");
    } else {
        payload["user"] = {{"id", user_id}};
    }

    if (payload.contains("backtrace") && payload["backtrace"].is_array()) {
        for (auto& frame : payload["backtrace"]) {
            if (frame.is_object()) {
                frame.erase("context_line");
                frame.erase("pre_context");
                frame.erase("post_context");
            }
        }
    }
    return payload;
}

nlohmann::json span_data(const nlohmann::json& data) {
    nlohmann::json kept = nlohmann::json::object();
    if (!data.is_object()) {
        return kept;
    }
    for (const char* key : {"db.statement", "db.system"}) {
        auto value = data.find(key);
        if (value != data.end()) {
            kept[key] = *value;
        }
    }
    return kept;
}

} // namespace strict_privacy
} // namespace forge_ops_tracker
