#pragma once

#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace forge_ops_tracker {

/**
 * An exception carrying the SQL statement that caused it. A C++ exception has no statement of its
 * own and no C++ database library puts one on its exception types, so the code that ran the query
 * has to attach it. Throw (or rethrow, from a catch of the driver's own exception) one of these
 * and everything downstream (capture_exception, the terminate handler) finds the statement with no
 * extra call:
 *
 *     try { session << query, soci::use(id); }
 *     catch (const std::exception& e) { throw forge_ops_tracker::SqlException(e.what(), query); }
 *
 * Or wrap the original with std::throw_with_nested and throw a SqlException around it, or report
 * the driver's own exception with capture_exception_with_sql and skip this class entirely.
 */
class SqlException : public std::runtime_error {
public:
    SqlException(const std::string& what, std::string statement)
        : std::runtime_error(what), statement_(std::move(statement)) {}

    const std::string& statement() const noexcept { return statement_; }

private:
    std::string statement_;
};

/**
 * Finds the SQL behind a database error and reduces it to something safe to send: the names of the
 * stored procedures, tables and views it touched, and (only if Configuration::capture_sql_statement
 * is on) the statement itself with every string and number replaced by "?". Ported from
 * gems/forge_ops_tracker's SqlStatement, which is itself ported from the server's own
 * SqlStatementMasker/SqlObjectExtractor: same rules everywhere, and the server applies them again
 * on arrival, so a difference here can only ever mean less is masked client-side, never that
 * something unmasked gets stored.
 *
 * Written as a hand-rolled scanner and tokenizer rather than std::regex: the shared pattern
 * relies on lookbehind (a number is only a value when it isn't part of an identifier) and a
 * backreference (a dollar-quoted body ends at the same tag that opened it), and ECMAScript-flavor
 * std::regex has neither. The rules are identical; only the mechanism differs.
 *
 * Deliberately not a SQL parser.
 */
namespace sql_statement {

constexpr std::size_t kMaxLength = 4000;

/** Replaces every string literal and number with "?". nullopt for a blank statement. */
std::optional<std::string> mask(const std::string& statement);

/**
 * Takes an already-masked statement (so a keyword inside a string value can't be mistaken for SQL)
 * and returns {"operation": ..., "procedures": [...], "relations": [...]}, or nullopt when nothing
 * recognizable was found. A view and a table are written the same way in SQL text, so both land in
 * "relations".
 */
std::optional<nlohmann::json> extract_objects(const std::string& masked);

/** The statement carried by `exception` or any exception nested inside it; empty when none does. */
std::string find_in(const std::exception& exception);

} // namespace sql_statement

} // namespace forge_ops_tracker
