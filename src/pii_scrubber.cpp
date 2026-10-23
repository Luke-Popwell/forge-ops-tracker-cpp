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

// Both lists are leaked rather than plain statics, like configuration.cpp's dsn_pattern(): a
// capture on another thread during exit must never find them destroyed.
const std::vector<std::string>& sensitive_keys() {
    static const std::vector<std::string>* const keys = new std::vector<std::string>{
        "password", "passwd", "pwd",
        "secret", "apisecret", "clientsecret", "secretkey",
        "token", "accesstoken", "refreshtoken", "apikey", "apitoken", "authorization", "authtoken", "bearer", "sessiontoken", "csrftoken",
        "creditcard", "cardnumber", "cardnum", "cvv", "cvv2", "cvc",
        "iban", "accountnumber", "routingnumber", "bankaccount",
        "ssn", "socialsecuritynumber", "socialsecurity",
        "privatekey",
    };
    return *keys;
}

struct Pattern {
    std::string label;
    std::regex regex;
};

// std::regex's default ECMAScript grammar accepts every one of these 8 patterns unmodified from
// app/services/pii_scrubber.rb's own Ruby syntax: verified directly against real matching
// input for each (see test_pii_scrubber.cpp), not assumed to translate cleanly just because the
// syntax looks the same.
const std::vector<Pattern>& patterns() {
    static const std::vector<Pattern>* const built = new std::vector<Pattern>([] {
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
    }());
    return *built;
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

// IBANs and card numbers in any shape: sdks/PII_SCRUBBING.md is the rule every SDK follows, and
// tests/fixtures/pii_scrubbing_vectors.json pins it. std::regex has no lookbehind, so both are
// found by hand, byte by byte, exactly as the server's backtracking engine finds them. Every
// character test is ASCII only, as the spec's `\b` is: a UTF-8 byte of a non-ASCII character is
// never a word character.
bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ascii_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_iban_char(char c) { return is_ascii_upper(c) || is_ascii_digit(c); }
bool is_word_char(char c) { return is_iban_char(c) || (c >= 'a' && c <= 'z') || c == '_'; }

// `\b` at pos (0 < pos <= s.size()).
bool is_word_boundary(const std::string& s, size_t pos) {
    return is_word_char(s[pos - 1]) != (pos < s.size() && is_word_char(s[pos]));
}

// Whether " " plus four [A-Z0-9] starts at pos.
bool is_iban_group(const std::string& s, size_t pos) {
    if (pos + 5 > s.size() || s[pos] != ' ') {
        return false;
    }
    for (size_t k = 1; k <= 4; k++) {
        if (!is_iban_char(s[pos + k])) {
            return false;
        }
    }
    return true;
}

// Where a match of \b[A-Z]{2}\d{2}(?:[A-Z0-9]{11,30}|(?: [A-Z0-9]{4}){2,7}(?: [A-Z0-9]{1,4})?)\b
// starting at start ends, or 0 when there's none: exactly what a backtracking engine returns,
// trying the compact branch first, every quantifier greedy, and giving back one repetition at a
// time until the closing \b holds.
size_t iban_candidate_end(const std::string& s, size_t start) {
    const size_t len = s.size();
    if (start > 0 && is_word_char(s[start - 1])) {
        return 0;
    }
    if (start + 4 > len || !is_ascii_upper(s[start]) || !is_ascii_upper(s[start + 1]) ||
        !is_ascii_digit(s[start + 2]) || !is_ascii_digit(s[start + 3])) {
        return 0;
    }
    const size_t rest = start + 4;

    size_t compact = 0;
    while (compact < 30 && rest + compact < len && is_iban_char(s[rest + compact])) {
        compact++;
    }
    for (size_t k = compact; k >= 11; k--) {
        if (is_word_boundary(s, rest + k)) {
            return rest + k;
        }
    }

    size_t groups = 0;
    while (groups < 7 && is_iban_group(s, rest + 5 * groups)) {
        groups++;
    }
    for (size_t g = groups; g >= 2; g--) {
        const size_t after = rest + 5 * g;
        if (after < len && s[after] == ' ') {
            size_t tail = 0;
            while (tail < 4 && after + 1 + tail < len && is_iban_char(s[after + 1 + tail])) {
                tail++;
            }
            for (size_t t = tail; t >= 1; t--) {
                if (is_word_boundary(s, after + 1 + t)) {
                    return after + 1 + t;
                }
            }
        }
        if (is_word_boundary(s, after)) {
            return after;
        }
    }
    return 0;
}

// The IBAN check, a digit at a time so no big integers are needed: with spaces removed, 15 to 34
// characters; the first four moved to the end, each letter read as two digits (A is 10, Z is 35),
// and the remainder by 97 is 1.
bool is_iban(const std::string& candidate) {
    std::string compact;
    for (char c : candidate) {
        if (c != ' ') {
            compact += c;
        }
    }
    if (compact.size() < 15 || compact.size() > 34) {
        return false;
    }
    unsigned remainder = 0;
    for (size_t k = 0; k < compact.size(); k++) {
        char c = compact[(k + 4) % compact.size()];
        if (is_ascii_upper(c)) {
            unsigned value = static_cast<unsigned>(c - 'A') + 10;
            remainder = (remainder * 10 + value / 10) % 97;
            remainder = (remainder * 10 + value % 10) % 97;
        } else {
            remainder = (remainder * 10 + static_cast<unsigned>(c - '0')) % 97;
        }
    }
    return remainder == 1;
}

// Every IBAN candidate, left to right and non-overlapping, replaced when it passes the check.
std::string scrub_ibans(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t copied = 0;
    for (size_t i = 0; i < text.size();) {
        size_t end = iban_candidate_end(text, i);
        if (end == 0) {
            i++;
            continue;
        }
        if (is_iban(text.substr(i, end - i))) {
            out.append(text, copied, i - copied);
            out += "[IBAN FILTERED]";
            copied = end;
        }
        i = end;
    }
    out.append(text, copied, std::string::npos);
    return out;
}

// 4, 51 to 55, 2221 to 2720, 34, 37, 6011, 644 to 649, 65, 3528 to 3589, 36, 300 to 305, 62.
// Only called with at least 13 digits.
bool has_card_prefix(const std::string& d) {
    int two = (d[0] - '0') * 10 + (d[1] - '0');
    int three = two * 10 + (d[2] - '0');
    int four = three * 10 + (d[3] - '0');
    return d[0] == '4' || (two >= 51 && two <= 55) || two == 34 || two == 37 || two == 65 || two == 36 ||
           two == 62 || (four >= 2221 && four <= 2720) || four == 6011 || (four >= 3528 && four <= 3589) ||
           (three >= 644 && three <= 649) || (three >= 300 && three <= 305);
}

// 13 to 19 digits, a card network's prefix, and the Luhn check.
bool is_card_number(const std::string& digits) {
    if (digits.size() < 13 || digits.size() > 19 || !has_card_prefix(digits)) {
        return false;
    }
    int sum = 0;
    for (size_t k = 0; k < digits.size(); k++) {
        int value = digits[digits.size() - 1 - k] - '0';
        if (k % 2 == 1) {
            value *= 2;
            if (value > 9) {
                value -= 9;
            }
        }
        sum += value;
    }
    return sum % 10 == 0;
}

// Splits a run (digits joined by single spaces or dashes) into its groups at each separator. From
// each group on, the longest stretch of whole groups that is a card number is replaced, and
// scanning carries on after it; with none, from the next group. So a card followed by its CVV
// ("4242424242424242 123"), two cards in a row, and Amex's 4-6-5 grouping are all found.
std::string scrub_card_numbers_in(const std::string& run) {
    std::vector<std::string> groups;
    std::vector<std::string> separators;
    size_t start = 0;
    for (size_t k = 0; k < run.size(); k++) {
        if (run[k] == ' ' || run[k] == '-') {
            groups.push_back(run.substr(start, k - start));
            separators.push_back(run.substr(k, 1));
            start = k + 1;
        }
    }
    groups.push_back(run.substr(start));
    separators.emplace_back();

    std::string out;
    size_t i = 0;
    while (i < groups.size()) {
        // A card number has at most 19 digits, so only stretches that short can be one; the last
        // one that is wins, the same as trying the longest stretch first.
        std::optional<size_t> found;
        std::string joined;
        for (size_t last = i; last < groups.size() && joined.size() + groups[last].size() <= 19; last++) {
            joined += groups[last];
            if (is_card_number(joined)) {
                found = last;
            }
        }
        if (found) {
            out += "[CREDIT CARD FILTERED]";
            out += separators[*found];
            i = *found + 1;
        } else {
            out += groups[i];
            out += separators[i];
            i++;
        }
    }
    return out;
}

// Finds what (?<![A-Za-z0-9_])\d(?:[ -]?\d)*(?![A-Za-z0-9_]) would, by hand: a run of digits
// joined by single spaces or dashes, starting at a digit not preceded by a word character. When
// the full run is followed by a word character, a regex engine backs off to just before the run's
// last separator (a separator is not a word character); with no separator to back off to, the run
// is no match at all and scanning carries on after it.
std::string scrub_card_numbers(const std::string& text) {
    const size_t len = text.size();
    std::string out;
    out.reserve(len);
    size_t copied = 0;
    size_t i = 0;
    while (i < len) {
        if (!is_ascii_digit(text[i]) || (i > 0 && is_word_char(text[i - 1]))) {
            i++;
            continue;
        }
        size_t end = i + 1;
        std::optional<size_t> last_separator;
        while (end < len) {
            if (is_ascii_digit(text[end])) {
                end++;
            } else if ((text[end] == ' ' || text[end] == '-') && end + 1 < len && is_ascii_digit(text[end + 1])) {
                last_separator = end;
                end += 2;
            } else {
                break;
            }
        }
        if (end < len && is_word_char(text[end])) {
            if (!last_separator) {
                i = end;
                continue;
            }
            end = *last_separator;
        }
        out.append(text, copied, i - copied);
        out += scrub_card_numbers_in(text.substr(i, end - i));
        copied = end;
        i = end;
    }
    out.append(text, copied, std::string::npos);
    return out;
}

} // namespace

std::string scrub_string(const std::string& text) {
    // IBANs first: a spaced IBAN's middle groups look like a formatted card number.
    std::string result = scrub_ibans(text);
    for (const auto& pattern : patterns()) {
        result = std::regex_replace(result, pattern.regex, "[" + pattern.label + " FILTERED]");
    }
    // Then card numbers in any other shape.
    return scrub_card_numbers(result);
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
