#pragma once

#include <atomic>
#include <cstdint>
#include "order.hpp"

class NetworkServer {
 public:
    NetworkServer(uint16_t port,
                  void (*push)(void*, const Order&),
                  void* ctx);

    bool start();
    void run(std::atomic<bool>& stop);
    uint64_t invalidCount() const noexcept { return invalid_count_; }

 private:
    uint16_t port_;
    void (*push_)(void*, const Order&);
    void* ctx_;
    int listen_fd_{-1};
    uint64_t invalid_count_{0};
};
