#pragma once

#include <chrono>
#include <cstdint>

using OrderId = std::unit64_t;
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

  Timestamp submit_time;
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
