#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "forge_ops_tracker/client.hpp"
#include "forge_ops_tracker/configuration.hpp"

namespace forge_ops_tracker {

/**
 * Times work in-process, bucketed by transaction name (see record_performance/time_transaction in
 * forge_ops_tracker.hpp), and periodically flushes each distinct bucket as one small aggregate
 * report, rather than one network call per timed call. Ported from
 * gems/forge_ops_tracker/lib/forge_ops_tracker/performance_flusher.rb and sdks/go's own port of it,
 * including the one thing both of those learned the hard way: see flush()'s own comment on why it
 * subtracts what it delivered instead of clearing the buckets.
 *
 * One std::thread, started lazily on the first recorded duration (the same lazy start
 * DeliveryQueue uses), waiting on a condition variable between flushes rather than sleeping, so
 * stopping it never has to wait out a full interval. The destructor flushes whatever is left (the
 * C++ equivalent of the Ruby gem's at_exit hook, since a global's destructor runs at normal exit),
 * unless discard() was called first.
 */
class PerformanceFlusher {
public:
    PerformanceFlusher(const Configuration& configuration, Client client);
    ~PerformanceFlusher();

    PerformanceFlusher(const PerformanceFlusher&) = delete;
    PerformanceFlusher& operator=(const PerformanceFlusher&) = delete;

    /** Does nothing (and starts no thread) when track_performance is off or reporting isn't enabled for this environment. */
    void record(const std::string& transaction_name, double duration_ms);

    /**
     * Snapshots the buffered buckets and delivers them as one batch. A failed delivery keeps every
     * bucket where it is, so the next flush's batch just grows instead of losing what was already
     * tallied: there's no other copy of this data anywhere.
     *
     * Only exactly what this snapshot delivered is removed afterward, subtracted from whatever is
     * in each bucket by then, never the whole map cleared outright. record() can run on another
     * thread while delivery is in flight (the lock is deliberately released around the network
     * call), so a record for a transaction already in the snapshot, or a brand-new one, can land in
     * the exact window between the snapshot and delivery succeeding. Clearing afterward, as if
     * delivery had covered everything now in the map, would silently discard that data forever.
     * This is a real bug sdks/go had and fixed, and that gems/forge_ops_tracker's reference
     * implementation still has; see this SDK's own test for a deterministic reproduction.
     */
    void flush();

    /** Stops the thread and drops every bucket without delivering anything. */
    void discard();

    struct Tally {
        unsigned long count = 0;
        double duration_sum_ms = 0;
        double max_duration_ms = 0;
    };

    /** @internal not part of the public API: current tally for one transaction (count 0 if none). */
    Tally tally_for_testing(const std::string& transaction_name);

    /**
     * @internal not part of the public API: called between the snapshot and the delivery inside
     * flush(), with no lock held, so a test can record "concurrently" at exactly the moment the
     * race window is open without needing a second thread. An empty function clears it.
     */
    void set_before_delivery_hook_for_testing(std::function<void()> hook);

private:
    struct Bucket {
        unsigned long count = 0;
        double duration_sum_ms = 0;
        double max_duration_ms = 0;
        /* A count per latency bucket label, see histogram_bucketer.hpp. */
        std::map<std::string, unsigned long> histogram;
    };

    const Configuration& configuration_;
    Client client_;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::map<std::string, Bucket> buckets_;
    std::chrono::system_clock::time_point period_started_at_ = std::chrono::system_clock::now();
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
