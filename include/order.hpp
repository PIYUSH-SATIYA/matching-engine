#pragma once

#include <chrono>
#include <cstdint>

using OrderId = std::uint64_t;
using TradeId = std::uint64_t;

using Price = std::int64_t;
using Quantity = std::uint64_t;

using Timestamp = std::chrono::steady_clock::time_point;

enum class Side : std::uint8_t { Buy, Sell };

enum class EventType : std::uint8_t {
  OrderAccepted,
  OrderRejected,
  TradeExecuted,
  OrderPartiallyFilled,
  OrderFullyFilled,
};

struct Order {
  OrderId id;
  Side side;

  Price price;
  Quantity quantity;

  // Stamped by the client when the order is sent (CLOCK_MONOTONIC). Latency is
  // measured from here, so it includes send, loopback TCP and ingress.
  Timestamp submit_time;

  // Stamped by the network thread when it has parsed the message and is about
  // to enqueue it. submit_time -> enqueue_time is the arrival component;
  // enqueue_time -> completion is the queue + matching component. Keeping both
  // turns "where is the tail?" into a measurement instead of an argument.
  Timestamp enqueue_time{};
};

struct Trade {
  TradeId id;

  OrderId buy_order_id;
  OrderId sell_order_id;

  Price price;
  Quantity quantity;

  Timestamp execution_time;
};

struct EngineEvent {
  EventType type;

  Timestamp timestamp;

  OrderId order_id;
  TradeId trade_id;

  Quantity quantity;
  Price price;
};
