#pragma once

#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/configuration.hpp"

namespace forge_ops_tracker {

/**
 * Turns a caught exception into the payload shape the ingestion API expects. C++ exceptions carry
 * no stack trace of their own: unlike every interpreted-language SDK in this repo, there's
 * nothing analogous to Ruby's backtrace or Python's traceback attached to the exception object
 * itself. The backtrace here is captured via backtrace()/backtrace_symbols() (POSIX/glibc,
 * <execinfo.h>) at the moment build() is *called*, not at the moment the exception was thrown:
 * call it as close to the catch site as possible for the most accurate trace, the same honest
 * caveat PHP's client documents for its own capture-timing quirk.
 *
 * No source context, ever: every other SDK in this repo (that has real file/line info at all)
 * attaches a few lines of source around an in-app frame's culprit line, read off disk at
 * capture-time. That needs a real file path and line number to key the disk read off of, and this
 * class's own backtrace() never produces one: every frame's "line" is JSON null (see backtrace()'s
 * own comment). Configuration::capture_source_context still exists here, defaulting to true like
 * every other SDK, purely so a host app configuring this client sees the same option every other
 * SDK has; attach_source_context() is a documented no-op, not a partial or best-effort
 * implementation of something that can never actually run on this SDK's capture path.
 */
class EventBuilder {
public:
    explicit EventBuilder(const Configuration& configuration);

    /**
     * exception_ptr rather than std::exception&, so a non-std::exception throw (any type can be
     * thrown in C++) still builds a usable event. `user` is attached under a top-level "user" key
     * (omitted entirely when empty), never scrubbed: see scrub_payload's own comment for why.
     * `breadcrumbs` is a JSON array of {category,message,level,timestamp,data} entries, attached
     * under a top-level "breadcrumbs" key (omitted entirely when empty); message and data are
     * scrubbed, category/level/timestamp never are. `sql` is the raw statement behind the error when
     * the caller has one (see capture_exception_with_sql); a SqlException carries its own, found
     * automatically. Either way it's masked here before anything is attached: see sql_statement.hpp.
     */
    nlohmann::json build(const std::exception_ptr& exception_ptr, const nlohmann::json& context = nlohmann::json::object(), const nlohmann::json& user = nlohmann::json::object(), const nlohmann::json& breadcrumbs = nlohmann::json::array(), const std::string& sql = "");

    /** Convenience overload for the common case of already having caught a std::exception. */
    nlohmann::json build(const std::exception& exception, const nlohmann::json& context = nlohmann::json::object(), const nlohmann::json& user = nlohmann::json::object(), const nlohmann::json& breadcrumbs = nlohmann::json::array(), const std::string& sql = "");

private:
    const Configuration& configuration_;

    nlohmann::json backtrace() const;
    bool is_in_app(const std::string& image) const;
    nlohmann::json scrub_payload(nlohmann::json payload) const;
    void attach_sql(nlohmann::json& payload, const std::string& raw_statement) const;

    /**
     * A deliberate no-op: see this class's own header comment above for why. Still called from
     * backtrace() per frame, the same shape every other SDK's real implementation takes, so the
     * code here reads the same way as everywhere else in this repo even though it never actually
     * has anything to attach.
     */
    nlohmann::json attach_source_context(nlohmann::json frame) const;
};

} // namespace forge_ops_tracker
