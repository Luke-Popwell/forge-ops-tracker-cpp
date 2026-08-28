#pragma once

#include <exception>
#include <functional>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/configuration.hpp"

namespace forge_ops_tracker {

/** Configure the client. Call once at startup. `configure` receives a mutable reference to apply overrides to. */
Configuration& init(const std::function<void(Configuration&)>& configure = nullptr);

/** Report an exception you've already caught. */
void capture_exception(const std::exception_ptr& exception_ptr, const nlohmann::json& context = nlohmann::json::object());
void capture_exception(const std::exception& exception, const nlohmann::json& context = nlohmann::json::object());

/**
 * Installs a std::terminate handler that reports whatever's in flight (if it's a real exception --
 * see the .cpp for what "in flight" can mean when std::terminate is reached some other way) before
 * chaining to the previously-installed handler. std::terminate handlers are documented as not
 * being permitted to return, so this SDK cannot resume normal execution afterward any more than
 * the default handler could -- the process will still terminate, the same way it would without
 * this installed; this only adds a report on the way out.
 */
void install_terminate_handler();

/** @internal not part of the public API -- resets module state between test cases */
void reset_for_testing();

} // namespace forge_ops_tracker
