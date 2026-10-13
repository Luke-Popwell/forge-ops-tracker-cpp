#include "forge_ops_tracker/client.hpp"

#include <algorithm>

#include <curl/curl.h>

namespace forge_ops_tracker {

namespace {
// libcurl calls this to hand over the response body: this client doesn't need it (only the
// status code matters, checked via CURLINFO_RESPONSE_CODE below), but a write callback must be
// set to something other than curl's own default (which writes to stdout) or a successful
// response would otherwise print its body to the host app's console.
std::size_t discard_response_body(char*, std::size_t size, std::size_t nmemb, void*) {
    return size * nmemb;
}

bool delivered(long status) {
    return delivery_outcome_for_status(status) == DeliveryOutcome::delivered;
}
} // namespace

DeliveryOutcome delivery_outcome_for_status(long status) {
    if (status >= 200 && status < 300) {
        return DeliveryOutcome::delivered;
    }
    if (status >= 400 && status < 500 && status != 408 && status != 429) {
        return DeliveryOutcome::rejected;
    }
    return DeliveryOutcome::failed;
}

std::string delivery_drop_reason(long status) {
    if (delivery_outcome_for_status(status) == DeliveryOutcome::rejected) {
        return "rejected by the server (HTTP " + std::to_string(status) + ")";
    }
    if (status == 0) {
        return "the server could not be reached";
    }
    return "the server answered HTTP " + std::to_string(status);
}

Client::Client(const Configuration& configuration) : configuration_(configuration) {}

bool Client::deliver(const nlohmann::json& payload) const {
    return delivered(post(configuration_.ingestion_uri(), payload));
}

bool Client::deliver(const nlohmann::json& payload, std::chrono::milliseconds max_time) const {
    return delivered(post(configuration_.ingestion_uri(), payload, max_time));
}

bool Client::deliver_performance_samples(const nlohmann::json& samples) const {
    return delivered(performance_samples_status(samples));
}

bool Client::deliver_metrics(const nlohmann::json& entries) const {
    return delivered(metrics_status(entries));
}

bool Client::deliver_infrastructure_metrics(const nlohmann::json& entries) const {
    return delivered(infrastructure_metrics_status(entries));
}

bool Client::deliver_spans(const nlohmann::json& trace) const {
    return delivered(post(configuration_.spans_uri(), trace));
}

bool Client::deliver_change(const nlohmann::json& change) const {
    return delivered(post(configuration_.changes_uri(), change));
}

long Client::performance_samples_status(const nlohmann::json& samples) const {
    return post(configuration_.performance_samples_uri(), nlohmann::json{{"samples", samples}});
}

long Client::metrics_status(const nlohmann::json& entries) const {
    return post(configuration_.custom_metrics_uri(), nlohmann::json{{"metrics", entries}});
}

long Client::infrastructure_metrics_status(const nlohmann::json& entries) const {
    return post(configuration_.infrastructure_metrics_uri(), nlohmann::json{{"metrics", entries}});
}

long Client::post(const std::optional<std::string>& uri, const nlohmann::json& payload,
                  std::optional<std::chrono::milliseconds> max_time) const {
    auto api_key = configuration_.api_key();
    if (!uri || !api_key) {
        return 0;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return 0;
    }

    std::string body = payload.dump();
    std::string auth_header = "Authorization: Bearer " + *api_key;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, uri->c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(configuration_.timeout_seconds));
    if (max_time) {
        // A timeout_seconds of 0 means no limit to curl, so it's not a cap to compare against.
        auto configured = std::chrono::milliseconds(std::chrono::seconds(configuration_.timeout_seconds));
        auto cap = configuration_.timeout_seconds > 0 ? std::min(*max_time, configured) : *max_time;
        // Set after CURLOPT_TIMEOUT, so this one wins: curl uses whichever of the two was set last.
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(std::max<long long>(1, cap.count())));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response_body);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // safe to use from a background thread: see DeliveryQueue

    CURLcode result = curl_easy_perform(curl);

    long status_code = 0;
    if (result == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    } else {
        configuration_.log(std::string("[forge-ops-tracker] delivery failed: ") + curl_easy_strerror(result));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status_code;
}

} // namespace forge_ops_tracker
