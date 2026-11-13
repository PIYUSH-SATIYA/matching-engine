#pragma once

// Shared TCP wire format (little-endian, x86-64 host layout).
//
// 40 bytes: u64 id, u8 side, 7 bytes padding, i64 price, u64 quantity,
// u64 client_ts_ns.  The client timestamp is CLOCK_MONOTONIC nanoseconds
// (std::chrono::steady_clock).  Client and server MUST run on the same host
// for these timestamps to be comparable.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "order.hpp"

namespace wire {

constexpr std::size_t kMsgSize = 40;

#pragma pack(push, 1)
struct Msg {
    std::uint64_t id;
    std::uint8_t  side;        // 0 = Buy, 1 = Sell
    std::uint8_t  pad[7];
    std::int64_t  price;
    std::uint64_t quantity;
    std::uint64_t client_ts_ns;
};
#pragma pack(pop)

static_assert(sizeof(Msg) == kMsgSize, "wire::Msg must be 40 bytes");

inline bool valid(const Msg& m) noexcept {
    return m.id != 0 && m.side <= 1 && m.quantity > 0;
}

inline Order toOrder(const Msg& m) noexcept {
    Order o{};
    o.id = m.id;
    o.side = (m.side == 0) ? Side::Buy : Side::Sell;
    o.price = static_cast<Price>(m.price);
    o.quantity = static_cast<Quantity>(m.quantity);
    o.submit_time = Timestamp(std::chrono::nanoseconds(m.client_ts_ns));
    return o;
}

// Parse as many complete messages as possible from `buf[0..used)`.
// Calls on_msg(const Msg&) for each valid message.  Moves any trailing
// partial message to the front of the buffer and returns the new used size.
template <class Fn>
inline std::size_t parse(char* buf, std::size_t used, std::uint64_t& invalid,
                         Fn&& on_msg) {
    std::size_t offset = 0;
    while (used - offset >= kMsgSize) {
        Msg m;
        std::memcpy(&m, buf + offset, kMsgSize);
        offset += kMsgSize;
        if (!valid(m)) {
            ++invalid;
            continue;
        }
        on_msg(m);
    }
    const std::size_t remaining = used - offset;
    if (remaining > 0 && offset > 0) {
        std::memmove(buf, buf + offset, remaining);
    }
    return remaining;
}

}  // namespace wire
