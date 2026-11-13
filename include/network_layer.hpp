#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "order.hpp"

// Counters used for the mechanism analysis in the report.  They are cheap
// increments on the network thread and are read after the thread joins.
struct NetStats {
    std::uint64_t accepts = 0;
    std::uint64_t recv_calls = 0;   // epoll: recv() syscalls; uring: recv SQEs
    std::uint64_t epoll_waits = 0;  // epoll only
    std::uint64_t submits = 0;      // io_uring submit calls (uring only)
    std::uint64_t completions = 0;  // io_uring CQEs (uring only)
    std::uint64_t bytes = 0;
    std::uint64_t invalid = 0;
    // Optional ingress latency probe (ME_INGEST_STATS=1).  An order can be old
    // before the engine ever sees it: client pacing, TCP delivery, or the
    // network thread being descheduled.  Splitting total latency into
    // "arrival delay" and "queue + match" is what tells us where a tail lives.
    std::uint64_t ingest_slow = 0;    // orders older than 1 ms when parsed
    std::uint64_t ingest_max_ns = 0;  // oldest parsed order, in ns
};

inline bool ingestStatsEnabled() {
    static const bool enabled = std::getenv("ME_INGEST_STATS") != nullptr;
    return enabled;
}

inline void noteIngest(NetStats& stats, std::int64_t client_ts_ns,
                       std::int64_t now_ns) {
    if (client_ts_ns <= 0) return;
    const std::int64_t delta = now_ns - client_ts_ns;
    if (delta <= 0) return;
    const auto d = static_cast<std::uint64_t>(delta);
    if (d > stats.ingest_max_ns) stats.ingest_max_ns = d;
    if (d > 1'000'000) ++stats.ingest_slow;
}


class INetworkServer {
 public:
    virtual ~INetworkServer() = default;
    virtual bool start() = 0;
    virtual void run(std::atomic<bool>& stop) = 0;
    virtual NetStats stats() const = 0;
};

using PushFn = void (*)(void*, const Order&);

std::unique_ptr<INetworkServer> makeEpollServer(std::uint16_t port, PushFn push,
                                                void* ctx,
                                                bool exit_on_disconnect = false);
std::unique_ptr<INetworkServer> makeUringServer(std::uint16_t port, PushFn push,
                                                void* ctx,
                                                bool exit_on_disconnect = false);
