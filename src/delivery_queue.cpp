#include "forge_ops_tracker/delivery_queue.hpp"

namespace forge_ops_tracker {

DeliveryQueue::DeliveryQueue(const Configuration& configuration, Client client)
    : configuration_(configuration), client_(std::move(client)) {}

DeliveryQueue::~DeliveryQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool DeliveryQueue::push(nlohmann::json payload) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= std::max<std::size_t>(1, configuration_.queue_size)) {
            configuration_.log("[forge-ops-tracker] delivery queue full, dropping event");
            return false;
        }
        queue_.push_back(std::move(payload));
    }
    ensure_worker_started();
    condition_.notify_one();
    return true;
}

void DeliveryQueue::ensure_worker_started() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_started_) {
        return;
    }
    worker_started_ = true;
    worker_ = std::thread(&DeliveryQueue::run, this);
}

void DeliveryQueue::run() {
    while (true) {
        nlohmann::json payload;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }
            payload = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            client_.deliver(payload);
        } catch (...) {
            // Per-item, not wrapping the whole loop: one bad delivery must not stop every event
            // queued after it. Client::deliver() is documented as non-throwing already, but this
            // guards the invariant regardless of what a future change to it might do.
            configuration_.log("[forge-ops-tracker] delivery worker caught an unexpected exception");
        }
    }
}

} // namespace forge_ops_tracker
