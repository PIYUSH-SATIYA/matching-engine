#include "order_book.hpp"

#include <iterator>

namespace {

// Erase one order from a side (bids or asks) and drop the level if it empties.
template <class SideMap, class It>
void eraseFromSide(SideMap& side, Price price, It it) {
    auto level = side.find(price);
    level->second.erase(it);
    if (level->second.empty()) {
        side.erase(level);
    }
}

}  // namespace

Order* OrderBook::frontAtBestBid() {
    if (bids_.empty()) return nullptr;
    return &bids_.begin()->second.front();
}

Order* OrderBook::frontAtBestAsk() {
    if (asks_.empty()) return nullptr;
    return &asks_.begin()->second.front();
}

bool OrderBook::add(const Order& order) {
    if (order.quantity == 0) return false;
    if (index_.count(order.id) != 0) return false;  // duplicate id

    Level::iterator it;
    if (order.side == Side::Buy) {
        Level& level = bids_[order.price];
        level.push_back(order);
        it = std::prev(level.end());
    } else {
        Level& level = asks_[order.price];
        level.push_back(order);
        it = std::prev(level.end());
    }
    index_.emplace(order.id, Locator{order.side, order.price, it});
    return true;
}

bool OrderBook::reduce(OrderId id, Quantity q) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;

    Order& resting = *found->second.it;
    if (q == 0 || q > resting.quantity) return false;

    if (q == resting.quantity) {
        eraseOrder(found->second);
        index_.erase(found);
    } else {
        resting.quantity -= q;  // keeps its place in the queue
    }
    return true;
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;

    eraseOrder(found->second);
    index_.erase(found);
    return true;
}

void OrderBook::eraseOrder(const Locator& loc) {
    if (loc.side == Side::Buy) {
        eraseFromSide(bids_, loc.price, loc.it);
    } else {
        eraseFromSide(asks_, loc.price, loc.it);
    }
}