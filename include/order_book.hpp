#pragma once

// Order Book - Version 1 (simple, correct reference implementation)
//
// Contract (phase-1-component-contract.md, section 9):
//   - Owned and mutated ONLY by the Matching Engine thread.
//   - No locking, no threads, no logging, no networking in here.
//   - Stores resting orders, keeps price-time priority.
//   - Does NOT decide matching. The Matching Engine inspects the best price,
//     reads the front order, and calls reduce()/cancel()/add().
//
// V1 internals: std::map<Price, std::list<Order>> per side + hash index by id.
// Intentionally simple. Later versions (intrusive list + slab, direct-indexed
// price array) must keep the SAME public interface so they can be swapped in
// and compared against this one.

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "order.hpp"

class OrderBook {
public:
    // ---- Inspect state -------------------------------------------------
    bool hasBid() const { return !bids_.empty(); }
    bool hasAsk() const { return !asks_.empty(); }

    // Precondition: hasBid() / hasAsk() is true.
    Price bestBid() const { return bids_.begin()->first; }
    Price bestAsk() const { return asks_.begin()->first; }

    // Oldest order at the best price (the one that must match first).
    // Returns nullptr if that side is empty.
    // The pointer stays valid until that order is reduced to zero or cancelled.
    Order* frontAtBestBid();
    Order* frontAtBestAsk();

    bool        contains(OrderId id) const { return index_.count(id) != 0; }
    std::size_t orderCount() const { return index_.size(); }
    std::size_t bidLevelCount() const { return bids_.size(); }
    std::size_t askLevelCount() const { return asks_.size(); }

    // ---- Mutate state (Matching Engine thread only) ---------------------

    // Rest an order at the BACK of its price level (time priority).
    // Returns false (book unchanged) if quantity == 0 or the id already exists.
    bool add(const Order& order);

    // Reduce a resting order's quantity (partial or full fill).
    // The order keeps its place in the queue.
    // If quantity reaches 0 the order is removed (and the level if it becomes empty).
    // Returns false if the id is unknown, q == 0, or q > resting quantity.
    bool reduce(OrderId id, Quantity q);

    // Remove a resting order completely. Returns false if the id is unknown.
    bool cancel(OrderId id);

private:
    using Level  = std::list<Order>;  // front = oldest. list => stable addresses
    using BidMap = std::map<Price, Level, std::greater<Price>>;  // best = highest
    using AskMap = std::map<Price, Level>;                       // best = lowest

    struct Locator {
        Side           side;
        Price          price;
        Level::iterator it;
    };

    void eraseOrder(const Locator& loc);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Locator> index_;  // id -> where it rests
};