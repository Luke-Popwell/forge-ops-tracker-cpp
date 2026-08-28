#pragma once

#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/configuration.hpp"

namespace forge_ops_tracker {

/**
 * Turns a caught exception into the payload shape the ingestion API expects. C++ exceptions carry
 * no stack trace of their own -- unlike every interpreted-language SDK in this repo, there's
 * nothing analogous to Ruby's backtrace or Python's traceback attached to the exception object
 * itself. The backtrace here is captured via backtrace()/backtrace_symbols() (POSIX/glibc,
 * <execinfo.h>) at the moment build() is *called*, not at the moment the exception was thrown --
 * call it as close to the catch site as possible for the most accurate trace, the same honest
 * caveat PHP's client documents for its own capture-timing quirk.
 */
class EventBuilder {
public:
    explicit EventBuilder(const Configuration& configuration);

    /** exception_ptr rather than std::exception&, so a non-std::exception throw (any type can be thrown in C++) still builds a usable event. */
    nlohmann::json build(const std::exception_ptr& exception_ptr, const nlohmann::json& context = nlohmann::json::object());

    /** Convenience overload for the common case of already having caught a std::exception. */
    nlohmann::json build(const std::exception& exception, const nlohmann::json& context = nlohmann::json::object());

private:
    const Configuration& configuration_;

    nlohmann::json backtrace() const;
    bool is_in_app(const std::string& image) const;
    nlohmann::json scrub_payload(nlohmann::json payload) const;
};

} // namespace forge_ops_tracker
