#pragma once

#include <exception>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/configuration.hpp"
#include "forge_ops_tracker/delivery_queue.hpp"
#include "forge_ops_tracker/event_builder.hpp"

namespace forge_ops_tracker {

/**
 * Ties Configuration, EventBuilder, and DeliveryQueue together into the one thing callers
 * actually need: report an exception. Mirrors gems/forge_ops_tracker's ErrorSubscriber#report:
 * never throws. An error reporter that itself throws while reporting an error is the worst
 * possible failure mode, so every path here is wrapped to guarantee this never propagates back
 * into the host app.
 */
class Reporter {
public:
    Reporter(const Configuration& configuration, EventBuilder event_builder, DeliveryQueue& delivery_queue);

    void report(const std::exception_ptr& exception_ptr, const nlohmann::json& context = nlohmann::json::object(), const nlohmann::json& user = nlohmann::json::object(), const nlohmann::json& breadcrumbs = nlohmann::json::array());
    void report(const std::exception& exception, const nlohmann::json& context = nlohmann::json::object(), const nlohmann::json& user = nlohmann::json::object(), const nlohmann::json& breadcrumbs = nlohmann::json::array());

private:
    const Configuration& configuration_;
    EventBuilder event_builder_;
    DeliveryQueue& delivery_queue_;
};

} // namespace forge_ops_tracker
