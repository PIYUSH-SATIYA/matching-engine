#pragma once

#include <cstddef>
#include <cstdio>
#include <vector>
#include "order.hpp"
#include "../src/event_manager.hpp"

// --- TradeLogSink -----------------------------------------------------------
struct TradeLogEntry {
    EventType   type;
    Timestamp   timestamp;
    OrderId     order_id;
    TradeId     trade_id;
    Quantity    quantity;
    Price       price;
};

class TradeLogSink {
 public:
    explicit TradeLogSink(std::size_t capacity) : capacity_(capacity) {
        entries_.reserve(capacity);
    }

    void onEvent(const EngineEvent& e) {
        if (entries_.size() >= capacity_) return;
        if (e.type == EventType::TradeExecuted || e.type == EventType::OrderRejected) {
            entries_.push_back({e.type, e.timestamp, e.order_id, e.trade_id, e.quantity, e.price});
        }
    }

    void onTrade(const Trade&) {}

    void flushToFile(const char* path) const {
        FILE* f = std::fopen(path, "w");
        if (!f) return;
        std::fprintf(f, "type,order_id,trade_id,quantity,price\n");
        for (const auto& en : entries_) {
            const char* tname = (en.type == EventType::TradeExecuted) ? "TradeExecuted" : "OrderRejected";
            std::fprintf(f, "%s,%llu,%llu,%llu,%lld\n",
                         tname,
                         (unsigned long long)en.order_id,
                         (unsigned long long)en.trade_id,
                         (unsigned long long)en.quantity,
                         (long long)en.price);
        }
        std::fclose(f);
    }

 private:
    std::vector<TradeLogEntry> entries_;
    std::size_t capacity_;
};

// --- EventManagerSink -------------------------------------------------------
class EventManagerSink {
 public:
    explicit EventManagerSink(EventManager& em) : em_(em) {}

    void onEvent(const EngineEvent& e) { em_.publish(e); }
    void onTrade(const Trade&) {}

 private:
    EventManager& em_;
};

// --- TeeSink<A,B> -----------------------------------------------------------
template <class A, class B>
class TeeSink {
 public:
    TeeSink(A& a, B& b) : a_(a), b_(b) {}

    void onEvent(const EngineEvent& e) {
        a_.onEvent(e);
        b_.onEvent(e);
    }
    void onTrade(const Trade& t) {
        a_.onTrade(t);
        b_.onTrade(t);
    }

 private:
    A& a_;
    B& b_;
};
