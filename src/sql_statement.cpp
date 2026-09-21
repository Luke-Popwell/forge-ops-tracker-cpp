#include "forge_ops_tracker/sql_statement.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

namespace forge_ops_tracker {
namespace sql_statement {

namespace {

constexpr const char* kMask = "?";
constexpr std::size_t kMaxNames = 10;
constexpr std::size_t kMaxNameLength = 200;
constexpr int kMaxCauseDepth = 5;

bool is_word(unsigned char c) { return c == '_' || std::isalnum(c) != 0; }
bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool is_alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_space(unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// Where the number starting at `i` ends, or -1 when it isn't a standalone number (digits
// immediately followed by a letter or underscore). A decimal that fails that check falls back to
// just its integer part, the same way the shared pattern's backtracking does.
long number_end(const std::string& s, std::size_t i) {
    std::size_t k = i;
    while (k < s.size() && is_digit(s[k])) {
        ++k;
    }
    const std::size_t int_end = k;
    if (k + 1 < s.size() && s[k] == '.' && is_digit(s[k + 1])) {
        std::size_t m = k + 1;
        while (m < s.size() && is_digit(s[m])) {
            ++m;
        }
        if (m >= s.size() || !is_word(s[m])) {
            return static_cast<long>(m);
        }
    }
    if (int_end >= s.size() || !is_word(s[int_end])) {
        return static_cast<long>(int_end);
    }
    return -1;
}

bool is_bare_char(unsigned char c) { return is_word(c) || c == '$' || c == '#' || c == '@'; }

// Where the identifier part starting at `i` ends, or -1: bare (letters, digits, _ $ # @), "double
// quoted", [bracketed] (SQL Server) or `backticked`.
long name_part_end(const std::string& s, std::size_t i) {
    if (i >= s.size()) {
        return -1;
    }
    const unsigned char c = s[i];
    if (is_bare_char(c)) {
        std::size_t j = i;
        while (j < s.size() && is_bare_char(s[j])) {
            ++j;
        }
        return static_cast<long>(j);
    }
    if (c == '"' || c == '`' || c == '[') {
        const char closer = c == '[' ? ']' : static_cast<char>(c);
        std::size_t j = i + 1;
        while (j < s.size() && s[j] != closer) {
            ++j;
        }
        if (j < s.size() && j > i + 1) {
            return static_cast<long>(j + 1);
        }
    }
    return -1;
}

// Where the (optionally schema-qualified) name starting at `i` ends, or -1.
long name_end(const std::string& s, std::size_t i) {
    long end = name_part_end(s, i);
    if (end < 0) {
        return -1;
    }
    while (static_cast<std::size_t>(end) < s.size() && s[end] == '.') {
        long next = name_part_end(s, static_cast<std::size_t>(end) + 1);
        if (next < 0) {
            break;
        }
        end = next;
    }
    return end;
}

struct Token {
    std::string text;
    bool is_name;
};

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < s.size()) {
        if (is_space(s[i])) {
            ++i;
            continue;
        }
        long end = name_end(s, i);
        if (end >= 0) {
            tokens.push_back({s.substr(i, static_cast<std::size_t>(end) - i), true});
            i = static_cast<std::size_t>(end);
            continue;
        }
        // Any other byte is its own token. A multi-byte UTF-8 character becomes several one-byte
        // tokens, which no keyword rule below ever matches, so it's harmless.
        tokens.push_back({std::string(1, s[i]), false});
        ++i;
    }
    return tokens;
}

bool is_full_name(const std::string& name) {
    return !name.empty() && name_end(name, 0) == static_cast<long>(name.size());
}

std::vector<std::string> clean(const std::vector<std::string>& names) {
    std::vector<std::string> cleaned;
    for (const std::string& raw : names) {
        std::size_t first = 0;
        std::size_t last = raw.size();
        while (first < last && is_space(raw[first])) ++first;
        while (last > first && is_space(raw[last - 1])) --last;
        std::string name = raw.substr(first, std::min(last - first, kMaxNameLength));
        if (is_full_name(name) && std::find(cleaned.begin(), cleaned.end(), name) == cleaned.end()) {
            cleaned.push_back(name);
        }
    }
    if (cleaned.size() > kMaxNames) {
        cleaned.resize(kMaxNames);
    }
    return cleaned;
}

const std::set<std::string> kOperations = {"SELECT", "INSERT", "UPDATE", "DELETE", "MERGE", "WITH", "CALL", "EXEC", "EXECUTE", "CREATE", "ALTER", "DROP", "TRUNCATE"};
const std::set<std::string> kBuiltins = {
    "count", "sum", "min", "max", "avg", "now", "coalesce", "nullif", "lower", "upper", "length", "concat",
    "cast", "date_trunc", "current_timestamp", "current_date", "row_number", "rank", "json_build_object",
    "json_agg", "array_agg"};
const std::set<std::string> kKeywordsNotNames = {"select", "set", "values", "where", "lateral", "only", "unnest", "generate_series"};

// SQLite/JDBC-style drivers put the statement after this marker in the exception text.
const std::string kCompilingMarker = "while compiling:";

} // namespace

std::optional<std::string> mask(const std::string& statement) {
    if (std::all_of(statement.begin(), statement.end(), [](unsigned char c) { return is_space(c); })) {
        return std::nullopt;
    }

    const std::string& s = statement;
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = s[i];
        if (c == '\'') {
            // A string literal; '' is an escaped quote. One cut off by truncation (no closing
            // quote) is masked to the end of the statement, never left half-visible.
            std::size_t j = i + 1;
            while (j < s.size()) {
                if (s[j] == '\'') {
                    if (j + 1 < s.size() && s[j + 1] == '\'') {
                        j += 2;
                        continue;
                    }
                    ++j;
                    break;
                }
                ++j;
            }
            out += kMask;
            i = j;
        } else if (c == '$') {
            // A dollar-quoted body ($tag$ ... $tag$): PostgreSQL function bodies and DO blocks.
            std::size_t j = i + 1;
            while (j < s.size() && (s[j] == '_' || is_alpha(s[j]))) {
                ++j;
            }
            if (j < s.size() && s[j] == '$') {
                const std::string tag = s.substr(i, j - i + 1);
                const std::size_t found = s.find(tag, j + 1);
                out += kMask;
                i = found == std::string::npos ? s.size() : found + tag.size();
            } else {
                out += static_cast<char>(c);
                ++i;
            }
        } else if (is_digit(c)) {
            // A number, unless it's part of an identifier (orders2, sp_v2), a $1 placeholder, or
            // the fraction of another number; those digits are left alone.
            const bool part_of_something = i > 0 && (is_word(s[i - 1]) || s[i - 1] == '$' || s[i - 1] == '.');
            const long end = part_of_something ? -1 : number_end(s, i);
            if (end >= 0) {
                out += kMask;
                i = static_cast<std::size_t>(end);
            } else {
                out += static_cast<char>(c);
                ++i;
            }
        } else {
            out += static_cast<char>(c);
            ++i;
        }
    }

    if (out.size() > kMaxLength) {
        // Cut back to a UTF-8 character boundary so the result is still valid text: a split
        // multi-byte character would make the JSON encoder throw and lose the whole event.
        std::size_t cut = kMaxLength;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out = out.substr(0, cut) + "...";
    }
    return out;
}

std::optional<nlohmann::json> extract_objects(const std::string& masked) {
    if (std::all_of(masked.begin(), masked.end(), [](unsigned char c) { return is_space(c); })) {
        return std::nullopt;
    }

    // EXTRACT(year FROM col), SUBSTRING(x FROM 2), TRIM(BOTH FROM x): a FROM that isn't a table.
    const std::vector<Token> all = tokenize(masked);
    std::vector<const Token*> tokens;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Token& t = all[i];
        const std::string word = lower(t.text);
        if (t.is_name && (word == "extract" || word == "substring" || word == "trim" || word == "overlay") &&
            i + 1 < all.size() && all[i + 1].text == "(") {
            long close = -1;
            for (std::size_t j = i + 2; j < all.size(); ++j) {
                if (all[j].text == "(") break;
                if (all[j].text == ")") {
                    close = static_cast<long>(j);
                    break;
                }
            }
            if (close >= 0) {
                i = static_cast<std::size_t>(close);
                continue;
            }
        }
        tokens.push_back(&t);
    }

    std::vector<std::string> procedures;
    std::vector<std::string> relations;

    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        const std::string word = lower(tokens[i]->text);
        if (tokens[i]->is_name && (word == "call" || word == "exec" || word == "execute" || word == "perform") && tokens[i + 1]->is_name) {
            const std::string& name = tokens[i + 1]->text;
            const std::string first = lower(name.substr(0, name.find('.')));
            if (first == "immediate" || first == "function" || first == "procedure") {
                continue;
            }
            procedures.push_back(name);
            ++i;
        }
    }

    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        const std::string keyword = lower(tokens[i]->text);
        if (!(tokens[i]->is_name && (keyword == "from" || keyword == "join" || keyword == "into" || keyword == "update" || keyword == "table") && tokens[i + 1]->is_name)) {
            continue;
        }
        const std::string name = tokens[i + 1]->text;
        const bool paren = i + 2 < tokens.size() && tokens[i + 2]->text == "(";
        ++i;
        if (kKeywordsNotNames.count(lower(name)) != 0) {
            continue;
        }
        // FROM/JOIN some_function(...) is a set-returning function (often a stored one), not a
        // table. INSERT INTO t (a, b) is just a column list, so INTO/UPDATE/TABLE never count.
        if (paren && (keyword == "from" || keyword == "join")) {
            procedures.push_back(name);
        } else {
            relations.push_back(name);
        }
    }

    if (tokens.size() >= 3 && tokens[0]->is_name && lower(tokens[0]->text) == "select" && tokens[1]->is_name &&
        tokens[2]->text == "(" && kBuiltins.count(lower(tokens[1]->text)) == 0) {
        const bool has_from = std::any_of(tokens.begin(), tokens.end(), [](const Token* t) { return t->is_name && lower(t->text) == "from"; });
        if (!has_from) {
            procedures.push_back(tokens[1]->text);
        }
    }

    std::string operation;
    if (!tokens.empty()) {
        const std::string& word = tokens[0]->text;
        std::size_t end = 0;
        while (end < word.size() && is_word(word[end])) ++end;
        const std::string candidate = upper(word.substr(0, end));
        if (kOperations.count(candidate) != 0) {
            operation = candidate;
        }
    }

    const std::vector<std::string> clean_procedures = clean(procedures);
    const std::vector<std::string> clean_relations = clean(relations);
    if (clean_procedures.empty() && clean_relations.empty() && operation.empty()) {
        return std::nullopt;
    }

    nlohmann::json result = nlohmann::json::object();
    if (!operation.empty()) {
        result["operation"] = operation;
    }
    result["procedures"] = clean_procedures;
    result["relations"] = clean_relations;
    return result;
}

namespace {

std::string find_in_at_depth(const std::exception& exception, int depth) {
    if (const auto* carrier = dynamic_cast<const SqlException*>(&exception)) {
        if (!carrier->statement().empty()) {
            return carrier->statement();
        }
    }

    // SQLite-style text: `..., while compiling: SELECT ...`.
    const std::string what = exception.what();
    const std::size_t marker = what.find(kCompilingMarker);
    if (marker != std::string::npos) {
        std::size_t start = marker + kCompilingMarker.size();
        while (start < what.size() && is_space(what[start])) ++start;
        std::size_t end = what.size();
        while (end > start && is_space(what[end - 1])) --end;
        if (end > start) {
            return what.substr(start, end - start);
        }
    }

    // Nested exceptions (std::throw_with_nested) are reached by rethrowing each one in turn, since
    // std::nested_exception only hands out an exception_ptr, never a reference.
    const auto* holder = dynamic_cast<const std::nested_exception*>(&exception);
    if (holder == nullptr || holder->nested_ptr() == nullptr || depth >= kMaxCauseDepth) {
        return "";
    }
    try {
        std::rethrow_exception(holder->nested_ptr());
    } catch (const std::exception& inner) {
        return find_in_at_depth(inner, depth + 1);
    } catch (...) {
        return "";
    }
}

} // namespace

std::string find_in(const std::exception& exception) {
    return find_in_at_depth(exception, 0);
}

} // namespace sql_statement
} // namespace forge_ops_tracker
