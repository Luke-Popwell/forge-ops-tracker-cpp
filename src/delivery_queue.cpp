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
    bool full = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        full = queue_.size() >= std::max<std::size_t>(1, configuration_.queue_size);
        if (!full) {
            queue_.push_back(std::move(payload));
        }
    }
    if (full) {
        // Logged outside the lock: a logger that throws must never leave it held, which would hang
        // the terminate handler's own push (and drain) on the way out.
        configuration_.log("[forge-ops-tracker] delivery queue full, dropping event");
        return false;
    }
    ensure_worker_started();
    condition_.notify_one();
    return true;
}

bool DeliveryQueue::drain(std::chrono::milliseconds timeout) noexcept {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + timeout;
    const auto remaining = [deadline] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
    };
    bool on_worker = false;

    try {
        while (true) {
            nlohmann::json payload;
            std::chrono::milliseconds left{0};
            // try_lock, never lock: on the way out of a crashing process the lock may be held by a
            // thread that will never run again.
            while (!mutex_.try_lock()) {
                if (clock::now() >= deadline) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            {
                std::lock_guard<std::mutex> lock(mutex_, std::adopt_lock);
                on_worker = worker_started_ && worker_.get_id() == std::this_thread::get_id();
                if (queue_.empty()) {
                    break;
                }
                // Checked before taking anything, so a payload left at the deadline stays queued
                // for the worker (or the destructor) rather than being dropped here.
                left = remaining();
                if (left.count() <= 0) {
                    return false;
                }
                payload = std::move(queue_.front());
                queue_.pop_front();
            }

            // Delivered here on the calling thread rather than handed to the worker: the worker may
            // be the thread that's crashing, or busy with a slow delivery of its own.
            client_.deliver(payload, left);
        }

        // The worker may still be sending something it took before this drain started. Not when
        // this is the worker: that delivery is the one it was interrupted in.
        while (!on_worker && in_flight_.load() > 0) {
            if (clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    } catch (...) {
        return false;
    }
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
            // Under the lock, together with the pop: drain() must never see the queue empty
            // while a payload is in neither the queue nor in_flight_.
            in_flight_.fetch_add(1);
        }

        try {
            client_.deliver(payload);
        } catch (...) {
            // Per-item, not wrapping the whole loop: one bad delivery must not stop every event
            // queued after it. Client::deliver() is documented as non-throwing already, but this
            // guards the invariant regardless of what a future change to it might do.
            in_flight_.fetch_sub(1);
            configuration_.log("[forge-ops-tracker] delivery worker caught an unexpected exception");
            continue;
        }
        in_flight_.fetch_sub(1);
    }
}

} // namespace forge_ops_tracker
