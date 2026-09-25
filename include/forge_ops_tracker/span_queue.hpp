#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/client.hpp"
#include "forge_ops_tracker/configuration.hpp"

namespace forge_ops_tracker {

/**
 * The same shape as DeliveryQueue, delivering finished traces to the spans endpoint: a small
 * bounded queue drained by one background std::thread, started lazily on the first push. push()
 * never blocks and drops a trace when the queue is full, since a request slow enough to be traced
 * must not be made slower by the tracker. The destructor drains what is queued (the C++ stand-in
 * for an at-exit hook, since a global's destructor runs at normal exit), unless discard() was called.
 *
 * `deliver` picks the endpoint; the default is Client::deliver_spans. record_change's queue passes
 * Client::deliver_change, so a change gets the same bound, thread and exit-time drain as a trace.
 */
class SpanQueue {
public:
    using Deliver = std::function<bool(const Client&, const nlohmann::json&)>;

    SpanQueue(const Configuration& configuration, Client client, Deliver deliver = nullptr);
    ~SpanQueue();

    SpanQueue(const SpanQueue&) = delete;
    SpanQueue& operator=(const SpanQueue&) = delete;

    bool push(nlohmann::json trace);

    /** Stops the thread and drops whatever is still queued without delivering it. */
    void discard();

private:
    const Configuration& configuration_;
    Client client_;
    Deliver deliver_;

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
