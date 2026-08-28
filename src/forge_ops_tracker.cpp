#include "forge_ops_tracker/forge_ops_tracker.hpp"

#include <cstdlib>
#include <memory>

#include "forge_ops_tracker/client.hpp"
#include "forge_ops_tracker/delivery_queue.hpp"
#include "forge_ops_tracker/reporter.hpp"

namespace forge_ops_tracker {

namespace {

std::unique_ptr<Configuration> g_configuration;
std::unique_ptr<DeliveryQueue> g_delivery_queue;
std::unique_ptr<Reporter> g_reporter;
std::terminate_handler g_previous_terminate_handler = nullptr;
bool g_terminate_handler_installed = false;

Configuration& configuration() {
    if (!g_configuration) {
        g_configuration = std::make_unique<Configuration>();
    }
    return *g_configuration;
}

Reporter& reporter() {
    if (!g_reporter) {
        g_delivery_queue = std::make_unique<DeliveryQueue>(configuration(), Client(configuration()));
        g_reporter = std::make_unique<Reporter>(configuration(), EventBuilder(configuration()), *g_delivery_queue);
    }
    return *g_reporter;
}

void handle_terminate() {
    // std::current_exception() is well-defined to return whatever exception caused termination
    // when std::terminate() was reached via an uncaught exception -- per the standard, not
    // assumed -- and returns a null exception_ptr otherwise (terminate() called directly, a
    // noexcept violation with nothing currently propagating, and so on), which report() below
    // already handles as "no exception_class/message available" the same way Reporter's
    // catch (...) branch does.
    std::exception_ptr current = std::current_exception();
    if (current) {
        reporter().report(current);
    }

    if (g_previous_terminate_handler) {
        g_previous_terminate_handler();
    }
    // A terminate handler is documented as not permitted to return -- fall through to the same
    // default behavior the process would have had without this installed, rather than returning
    // and leaving the process in an undefined state.
    std::abort();
}

} // namespace

Configuration& init(const std::function<void(Configuration&)>& configure) {
    Configuration& config = configuration();
    if (configure) {
        configure(config);
    }
    return config;
}

void capture_exception(const std::exception_ptr& exception_ptr, const nlohmann::json& context) {
    reporter().report(exception_ptr, context);
}

void capture_exception(const std::exception& exception, const nlohmann::json& context) {
    reporter().report(exception, context);
}

void install_terminate_handler() {
    if (g_terminate_handler_installed) {
        return;
    }
    g_terminate_handler_installed = true;
    g_previous_terminate_handler = std::set_terminate(&handle_terminate);
}

void reset_for_testing() {
    g_reporter.reset();
    g_delivery_queue.reset();
    g_configuration.reset();
    // Deliberately not touching std::set_terminate here -- resetting the real process-wide
    // terminate handler between test runs would risk leaving the *test binary itself* without
    // whatever handler it started with if an unrelated later test genuinely terminates.
}

} // namespace forge_ops_tracker
