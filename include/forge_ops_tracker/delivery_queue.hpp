#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/client.hpp"
#include "forge_ops_tracker/configuration.hpp"

namespace forge_ops_tracker {

/**
 * A small bounded queue drained by a background std::thread, so delivery never blocks the caller
 * that raised the error. The closest real equivalent to the Ruby/Java clients' own
 * background-thread DeliveryQueue: see gems/forge_ops_tracker/lib/forge_ops_tracker/delivery_queue.rb.
 * Bounded via `queue_size`; push() never blocks, drops and logs when full. The worker thread is
 * started lazily, on first push, not at construction: not for a fork-safety reason the way
 * Ruby/Python/Node's own lazy start is (a C++ binary doesn't have an interpreter-level module
 * load moment a prefork server could fork after), but simply so a Reporter that's constructed but
 * never actually used to report anything never spins up a thread it doesn't need.
 */
class DeliveryQueue {
public:
    DeliveryQueue(const Configuration& configuration, Client client);
    ~DeliveryQueue();

    DeliveryQueue(const DeliveryQueue&) = delete;
    DeliveryQueue& operator=(const DeliveryQueue&) = delete;

    bool push(nlohmann::json payload);

private:
    const Configuration& configuration_;
    Client client_;

    std::deque<nlohmann::json> queue_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool worker_started_ = false;
    bool stopping_ = false;

    void ensure_worker_started();
    void run();
};

} // namespace forge_ops_tracker
