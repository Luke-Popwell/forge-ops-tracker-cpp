#include "forge_ops_tracker/reporter.hpp"

namespace forge_ops_tracker {

Reporter::Reporter(const Configuration& configuration, EventBuilder event_builder, DeliveryQueue& delivery_queue)
    : configuration_(configuration), event_builder_(std::move(event_builder)), delivery_queue_(delivery_queue) {}

void Reporter::report(const std::exception_ptr& exception_ptr, const nlohmann::json& context, const nlohmann::json& user, const nlohmann::json& breadcrumbs, const std::string& sql, const std::optional<std::string>& trace_id) {
    try {
        if (!configuration_.is_enabled()) {
            return;
        }
        nlohmann::json payload = event_builder_.build(exception_ptr, context, user, breadcrumbs, sql, trace_id);
        delivery_queue_.push(std::move(payload));
    } catch (const std::exception& e) {
        configuration_.log(std::string("[forge-ops-tracker] report failed: ") + e.what());
    } catch (...) {
        configuration_.log("[forge-ops-tracker] report failed with a non-standard exception");
    }
}

void Reporter::report(const std::exception& exception, const nlohmann::json& context, const nlohmann::json& user, const nlohmann::json& breadcrumbs, const std::string& sql, const std::optional<std::string>& trace_id) {
    // Same slicing hazard as EventBuilder::build's own (const std::exception&, ...) overload, and
    // the same fix: see that function's comment in event_builder.cpp for the full reasoning.
    // Resolved here too, not just there, since Reporter is itself a public entry point (a caller
    // building its own Configuration/EventBuilder/DeliveryQueue/Reporter stack directly, rather
    // than going through the package-level capture_exception(), calls straight into this).
    std::exception_ptr current = std::current_exception();
    report(current ? current : std::make_exception_ptr(exception), context, user, breadcrumbs, sql, trace_id);
}

} // namespace forge_ops_tracker
