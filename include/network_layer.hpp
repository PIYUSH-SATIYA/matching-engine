#pragma once

#include <atomic>
#include <cstdint>
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
};

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
