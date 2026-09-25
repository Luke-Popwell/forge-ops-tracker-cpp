#include "forge_ops_tracker/client.hpp"

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
} // namespace

Client::Client(const Configuration& configuration) : configuration_(configuration) {}

bool Client::deliver(const nlohmann::json& payload) const {
    return post(configuration_.ingestion_uri(), payload);
}

bool Client::deliver_performance_samples(const nlohmann::json& samples) const {
    return post(configuration_.performance_samples_uri(), nlohmann::json{{"samples", samples}});
}

bool Client::deliver_metrics(const nlohmann::json& entries) const {
    return post(configuration_.custom_metrics_uri(), nlohmann::json{{"metrics", entries}});
}

bool Client::deliver_infrastructure_metrics(const nlohmann::json& entries) const {
    return post(configuration_.infrastructure_metrics_uri(), nlohmann::json{{"metrics", entries}});
}

bool Client::deliver_spans(const nlohmann::json& trace) const {
    return post(configuration_.spans_uri(), trace);
}

bool Client::deliver_change(const nlohmann::json& change) const {
    return post(configuration_.changes_uri(), change);
}

bool Client::post(const std::optional<std::string>& uri, const nlohmann::json& payload) const {
    auto api_key = configuration_.api_key();
    if (!uri || !api_key) {
        return false;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return false;
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response_body);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // safe to use from a background thread: see DeliveryQueue

    CURLcode result = curl_easy_perform(curl);

    bool success = false;
    if (result == CURLE_OK) {
        long status_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        success = status_code >= 200 && status_code < 300;
    } else {
        configuration_.log(std::string("[forge-ops-tracker] delivery failed: ") + curl_easy_strerror(result));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return success;
}

} // namespace forge_ops_tracker
