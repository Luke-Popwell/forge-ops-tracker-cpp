#include "forge_ops_tracker/performance_flusher.hpp"

#include <ctime>
#include <utility>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/histogram_bucketer.hpp"

namespace forge_ops_tracker {

namespace {

// gmtime_r, not std::gmtime: std::gmtime returns a pointer to one shared static buffer, which a
// flush on the worker thread and one on a caller's thread could race on.
std::string format_timestamp(std::chrono::system_clock::time_point when) {
    std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm utc{};
    gmtime_r(&t, &utc);
    char formatted[32];
    std::strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return formatted;
}

} // namespace

PerformanceFlusher::PerformanceFlusher(const Configuration& configuration, Client client)
    : configuration_(configuration), client_(std::move(client)) {}

PerformanceFlusher::~PerformanceFlusher() {
    stop();
    bool discarded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        discarded = discarded_;
    }
    if (!discarded) {
        try {
            flush();
        } catch (...) {
            // A destructor must never throw, least of all at process exit.
        }
    }
}

void PerformanceFlusher::record(const std::string& transaction_name, double duration_ms) {
    if (!configuration_.track_performance || !configuration_.is_enabled()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        Bucket& bucket = buckets_[transaction_name];
        bucket.count++;
        bucket.duration_sum_ms += duration_ms;
        if (duration_ms > bucket.max_duration_ms) {
            bucket.max_duration_ms = duration_ms;
        }
        // The distribution count/sum/max can't reconstruct: see histogram_bucketer.hpp for why the
        // server approximates a percentile from these bucket counts.
        bucket.histogram[histogram_bucket_for(duration_ms)]++;
    }
    ensure_worker_started();
}

void PerformanceFlusher::flush() {
    std::map<std::string, Bucket> snapshot;
    std::chrono::system_clock::time_point period_start;
    std::chrono::system_clock::time_point period_end;
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (buckets_.empty()) {
            return;
        }
        snapshot = buckets_;
        period_start = period_started_at_;
        period_end = std::chrono::system_clock::now();
        hook = before_delivery_hook_;
    }

    if (hook) {
        hook();
    }

    nlohmann::json samples = nlohmann::json::array();
    for (const auto& [name, bucket] : snapshot) {
        samples.push_back({
            {"transaction_name", name},
            {"environment", configuration_.environment},
            {"release", configuration_.release ? nlohmann::json(*configuration_.release) : nlohmann::json(nullptr)},
            {"period_started_at", format_timestamp(period_start)},
            {"period_ended_at", format_timestamp(period_end)},
            {"request_count", bucket.count},
            {"duration_sum_ms", bucket.duration_sum_ms},
            {"max_duration_ms", bucket.max_duration_ms},
            {"histogram", bucket.histogram},
        });
    }

    if (!client_.deliver_performance_samples(samples)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, sent] : snapshot) {
        auto it = buckets_.find(name);
        if (it == buckets_.end()) {
            continue;
        }
        Bucket& current = it->second;
        current.count = current.count > sent.count ? current.count - sent.count : 0;
        current.duration_sum_ms = std::max(0.0, current.duration_sum_ms - sent.duration_sum_ms);
        for (const auto& [label, sent_count] : sent.histogram) {
            auto label_it = current.histogram.find(label);
            if (label_it == current.histogram.end()) {
                continue;
            }
            if (label_it->second > sent_count) {
                label_it->second -= sent_count;
            } else {
                current.histogram.erase(label_it);
            }
        }
        // max_duration_ms is deliberately left as whatever is currently on the bucket, sent or not:
        // unlike count/duration_sum_ms, a max can't be correctly "subtracted" back out (the true
        // max of what's left is anything at or below it, not knowable from the two numbers alone),
        // and leaving it never overstates the next period's own max, only potentially understates
        // how far back it was actually set.
        if (current.count == 0) {
            buckets_.erase(it);
        }
    }
    period_started_at_ = period_end;
}

void PerformanceFlusher::discard() {
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    discarded_ = true;
    buckets_.clear();
}

PerformanceFlusher::Tally PerformanceFlusher::tally_for_testing(const std::string& transaction_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buckets_.find(transaction_name);
    if (it == buckets_.end()) {
        return {};
    }
    return {it->second.count, it->second.duration_sum_ms, it->second.max_duration_ms};
}

void PerformanceFlusher::set_before_delivery_hook_for_testing(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    before_delivery_hook_ = std::move(hook);
}

void PerformanceFlusher::ensure_worker_started() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_started_ || stopping_) {
        return;
    }
    worker_started_ = true;
    worker_ = std::thread(&PerformanceFlusher::run, this);
}

void PerformanceFlusher::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        auto interval = std::max(std::chrono::milliseconds(1), configuration_.performance_flush_interval);
        if (condition_.wait_for(lock, interval, [this] { return stopping_; })) {
            return;
        }

        lock.unlock();
        try {
            flush();
        } catch (...) {
            // One bad flush must not stop every flush after it: same reasoning DeliveryQueue's
            // own per-item catch documents.
            configuration_.log("[forge-ops-tracker] performance flush caught an unexpected exception");
        }
        lock.lock();
    }
}

void PerformanceFlusher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

} // namespace forge_ops_tracker
