#include "forge_ops_tracker/span_queue.hpp"

#include <algorithm>

namespace forge_ops_tracker {

SpanQueue::SpanQueue(const Configuration& configuration, Client client)
    : configuration_(configuration), client_(std::move(client)) {}

SpanQueue::~SpanQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void SpanQueue::discard() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool SpanQueue::push(nlohmann::json trace) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= std::max<std::size_t>(1, configuration_.queue_size)) {
            configuration_.log("[forge-ops-tracker] span queue full, dropping trace");
            return false;
        }
        queue_.push_back(std::move(trace));
    }
    ensure_worker_started();
    condition_.notify_one();
    return true;
}

void SpanQueue::ensure_worker_started() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_started_) {
        return;
    }
    worker_started_ = true;
    worker_ = std::thread(&SpanQueue::run, this);
}

void SpanQueue::run() {
    while (true) {
        nlohmann::json trace;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }
            trace = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            client_.deliver_spans(trace);
        } catch (...) {
            configuration_.log("[forge-ops-tracker] span worker caught an unexpected exception");
        }
    }
}

} // namespace forge_ops_tracker
