#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "forge_ops_tracker/client.hpp"
#include "forge_ops_tracker/configuration.hpp"

namespace forge_ops_tracker {

/**
 * Collects individual capture_metric/capture_infrastructure_metric calls in-process and
 * periodically flushes them as one batch, rather than one network call per capture. Unlike
 * PerformanceFlusher this keeps a list of individually meaningful entries instead of summing them
 * into buckets: a customer's own signup or payment is exactly the kind of thing they will want a
 * genuinely accurate count/sum of later, so the server stores one row per entry as-is. Ported from
 * gems/forge_ops_tracker's metric_buffer.rb and infrastructure_metric_buffer.rb, which are the same
 * class twice; here it is one class instantiated twice, told which delivery function and flush
 * interval to use.
 *
 * Three deliberate differences from the Ruby buffers:
 *
 *  - A flush snapshots the first N entries and, on success, removes exactly those N, instead of
 *    resetting the whole list, so an entry recorded while the request is in flight (the lock is
 *    released around the network call) is kept for the next flush rather than lost.
 *  - The buffer is capped at max_entries, and once full further entries are dropped until a flush
 *    succeeds: a plan without the feature answers 403 on every flush, and an uncapped buffer would
 *    then grow for as long as the process lives. Dropping the newest rather than the oldest keeps
 *    the entries a flush is delivering at the front of the list, which is what makes removing
 *    exactly them afterward exact.
 *  - A NaN or infinite value is dropped at record time: nlohmann::json would serialize it as null,
 *    which the server would reject, taking the whole batch with it.
 *
 * One std::thread, started lazily on the first record, waiting on a condition variable between
 * flushes so stopping it never has to wait out a full interval. The destructor flushes whatever is
 * left (the C++ equivalent of the Ruby gem's at_exit hook, since a global's destructor runs at normal
 * exit), which is what a short-lived cron program that captures a few readings and returns from main
 * relies on, unless discard() was called first.
 */
class MetricBuffer {
public:
    static constexpr std::size_t max_entries = 1000;

    /** `deliver` takes a batch and returns whether delivery succeeded; `interval` is read fresh each cycle. */
    MetricBuffer(const Configuration& configuration, std::function<bool(const nlohmann::json&)> deliver, std::function<std::chrono::milliseconds()> interval);
    ~MetricBuffer();

    MetricBuffer(const MetricBuffer&) = delete;
    MetricBuffer& operator=(const MetricBuffer&) = delete;

    /** Adds one entry (everything but recorded_at, which is stamped here); returns whether it was kept. */
    bool record(nlohmann::json entry);

    /** Delivers everything buffered so far as one batch. A failed delivery keeps every entry. */
    void flush();

    /** Stops the thread and drops everything buffered without delivering it. */
    void discard();

    /** @internal not part of the public API: how many entries are currently buffered. */
    std::size_t size_for_testing();

    /**
     * @internal not part of the public API: called between the snapshot and the delivery inside
     * flush(), with no lock held, so a test can record "concurrently" at exactly the moment the race
     * window is open without a second thread. An empty function clears it.
     */
    void set_before_delivery_hook_for_testing(std::function<void()> hook);

private:
    const Configuration& configuration_;
    std::function<bool(const nlohmann::json&)> deliver_;
    std::function<std::chrono::milliseconds()> interval_;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<nlohmann::json> entries_;
    std::thread worker_;
    bool worker_started_ = false;
    bool stopping_ = false;
    bool discarded_ = false;
    std::function<void()> before_delivery_hook_;

    void ensure_worker_started();
    void run();
    void stop();
};

} // namespace forge_ops_tracker
