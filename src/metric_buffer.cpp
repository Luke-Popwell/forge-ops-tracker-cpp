#include "forge_ops_tracker/metric_buffer.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <utility>

namespace forge_ops_tracker {

namespace {

// gmtime_r, not std::gmtime: std::gmtime returns a pointer to one shared static buffer, which a
// record on one thread and one on another could race on.
std::string now_timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&t, &utc);
    char formatted[32];
    std::strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return formatted;
}

} // namespace

MetricBuffer::MetricBuffer(const Configuration& configuration, std::function<bool(const nlohmann::json&)> deliver, std::function<std::chrono::milliseconds()> interval)
    : configuration_(configuration), deliver_(std::move(deliver)), interval_(std::move(interval)) {}

MetricBuffer::~MetricBuffer() {
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

bool MetricBuffer::record(nlohmann::json entry) {
    const auto value = entry.find("value");
    if (value == entry.end() || !value->is_number() || !std::isfinite(value->get<double>())) {
        configuration_.log("[forge-ops-tracker] dropped a metric with a non-numeric or non-finite value");
        return false;
    }

    entry["recorded_at"] = now_timestamp();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.size() >= max_entries) {
            configuration_.log("[forge-ops-tracker] metric buffer full, dropping a metric");
            return false;
        }
        entries_.push_back(std::move(entry));
    }
    ensure_worker_started();
    return true;
}

void MetricBuffer::flush() {
    nlohmann::json batch;
    std::size_t count;
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.empty()) {
            return;
        }
        count = entries_.size();
        batch = nlohmann::json(entries_);
        hook = before_delivery_hook_;
    }

    if (hook) {
        hook();
    }

    if (!deliver_(batch)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // Exactly the entries just delivered: anything recorded while the request was in flight sits
    // after them and stays for the next flush.
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(count));
}

void MetricBuffer::discard() {
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    discarded_ = true;
    entries_.clear();
}

std::size_t MetricBuffer::size_for_testing() {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void MetricBuffer::set_before_delivery_hook_for_testing(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    before_delivery_hook_ = std::move(hook);
}

void MetricBuffer::ensure_worker_started() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_started_ || stopping_) {
        return;
    }
    worker_started_ = true;
    worker_ = std::thread(&MetricBuffer::run, this);
}

void MetricBuffer::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        auto interval = std::max(std::chrono::milliseconds(1), interval_());
        if (condition_.wait_for(lock, interval, [this] { return stopping_; })) {
            return;
        }

        lock.unlock();
        try {
            flush();
        } catch (...) {
            configuration_.log("[forge-ops-tracker] metric flush caught an unexpected exception");
        }
        lock.lock();
    }
}

void MetricBuffer::stop() {
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
