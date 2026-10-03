#pragma once

#include <atomic>
#include <chrono>
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

    /**
     * Delivers whatever is still queued, on the calling thread, and waits for a delivery the
     * worker thread already has under way, all within `timeout`. Returns true when everything
     * went out in time. The process-exit counterpart to the destructor's unbounded drain, for a
     * process about to die without running destructors: the terminate handler calls it before
     * std::abort(), the way gems/forge_ops_tracker's DeliveryQueue#drain runs at exit.
     *
     * Safe to call from any thread, the worker thread included (it then doesn't wait on its own
     * delivery). It never blocks on the queue's lock, only try_locks it, so a lock some other
     * thread never releases makes it give up at the deadline rather than hang. Each delivery is cut
     * short at the deadline too. Never throws.
     */
    bool drain(std::chrono::milliseconds timeout) noexcept;

    /**
     * Stops the worker after it has delivered everything queued, and joins it: what the destructor
     * does, callable earlier. The exit hook forge_ops_tracker::init registers calls it, so the
     * worker is finished before static destruction starts tearing down what it uses. A push after
     * this is kept but never delivered (no new thread starts). Safe to call more than once.
     */
    void shutdown();

private:
    const Configuration& configuration_;
    Client client_;

    std::deque<nlohmann::json> queue_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool worker_started_ = false;
    bool stopping_ = false;
    // How many payloads the worker has taken off the queue but not finished delivering (0 or 1).
    std::atomic<int> in_flight_{0};

    void ensure_worker_started();
    void run();
};

} // namespace forge_ops_tracker
