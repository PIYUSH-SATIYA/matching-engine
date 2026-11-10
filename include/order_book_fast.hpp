#pragma once

// Order Book - optimized version (V3).
//
// SAME public interface as the V1 OrderBook (include/order_book.hpp), so the
// Matching Engine can use either one (it is a template parameter).
//
// What changed versus V1 and why it is faster:
//
//  V1                                   V3
//  ---------------------------------    ------------------------------------
//  std::map<Price, ...> (tree)          levels_[price - min_price]  direct array
//                                       -> no tree walk, no pointer chasing
//  std::list node = malloc per order    Node from SlabAllocator (pre-allocated,
//                                       one cache line each, no malloc/free)
//  std::unordered_map (malloc + rehash) open-addressing flat hash table,
//                                       pre-sized, never rehashes
//  begin() of the map = best price      cached best index + 2-level bitmap;
//                                       next best found with ctz/clz
//
// Trade-offs (be honest about these in the report):
//  * Prices must lie in [min_price, min_price + NumLevels). Outside that
//    range validPrice() is false and add() returns false.
//  * At most MaxOrders resting orders. When the slab is full add() returns
//    false; the Matching Engine must reject the order.
//  * Memory is reserved up front (about 64 B * MaxOrders for nodes).
//
// Threading: Matching Engine thread only (contract section 9). No locks.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "order.hpp"
#include "slab_allocator.hpp"

template <std::size_t MaxOrders = (1u << 20), std::size_t NumLevels = (1u << 16)>
class OrderBookFast {
  static_assert(NumLevels % 64 == 0 && NumLevels >= 64,
                "NumLevels must be a multiple of 64");
  static_assert(MaxOrders >= 1, "MaxOrders must be >= 1");

 public:
  explicit OrderBookFast(Price min_price = 0)
      : min_price_(min_price),
        slab_(std::make_unique<Slab>()),
        bids_(NumLevels),
        asks_(NumLevels),
        hash_(kHashSize) {}

  OrderBookFast(const OrderBookFast&) = delete;
  OrderBookFast& operator=(const OrderBookFast&) = delete;

  // ---- Inspect state -----------------------------------------------------
  bool hasBid() const { return best_bid_ != kNone; }
  bool hasAsk() const { return best_ask_ != kNone; }
  Price bestBid() const { return min_price_ + static_cast<Price>(best_bid_); }
  Price bestAsk() const { return min_price_ + static_cast<Price>(best_ask_); }

  Order* frontAtBestBid() {
    return best_bid_ == kNone ? nullptr : &bids_[best_bid_].head->order;
  }
  Order* frontAtBestAsk() {
    return best_ask_ == kNone ? nullptr : &asks_[best_ask_].head->order;
  }

  bool validPrice(Price p) const {
    return p >= min_price_ &&
           static_cast<std::uint64_t>(p - min_price_) < NumLevels;
  }
  bool contains(OrderId id) const { return findSlot(id) != kNone; }
  std::size_t orderCount() const { return slab_->allocated(); }
  std::size_t bidLevelCount() const { return bid_levels_; }
  std::size_t askLevelCount() const { return ask_levels_; }

  // ---- Mutate state (Matching Engine thread only) --------------------------
  bool add(const Order& order) {
    if (order.quantity == 0 || !validPrice(order.price)) return false;
    if (findSlot(order.id) != kNone) return false;  // duplicate id

    Node* n = slab_->allocate();
    if (n == nullptr) return false;  // pool exhausted
    new (n) Node{order, nullptr, nullptr};

    const std::size_t idx = static_cast<std::size_t>(order.price - min_price_);
    const bool is_buy = (order.side == Side::Buy);
    Level& lvl = (is_buy ? bids_ : asks_)[idx];

    n->prev = lvl.tail;
    if (lvl.tail) lvl.tail->next = n; else lvl.head = n;
    lvl.tail = n;

    if (n->prev == nullptr) {  // level was empty -> now non-empty
      if (is_buy) {
        bid_map_.set(idx);
        ++bid_levels_;
        if (best_bid_ == kNone || idx > best_bid_) best_bid_ = idx;
      } else {
        ask_map_.set(idx);
        ++ask_levels_;
        if (best_ask_ == kNone || idx < best_ask_) best_ask_ = idx;
      }
    }
    hashInsert(order.id, n);
    return true;
  }

  bool reduce(OrderId id, Quantity q) {
    const std::size_t slot = findSlot(id);
    if (slot == kNone) return false;
    Node* n = hash_[slot].node;
    if (q == 0 || q > n->order.quantity) return false;
    if (q == n->order.quantity) {
      remove(n, slot);
    } else {
      n->order.quantity -= q;  // keeps its place in the queue
    }
    return true;
  }

  bool cancel(OrderId id) {
    const std::size_t slot = findSlot(id);
    if (slot == kNone) return false;
    remove(hash_[slot].node, slot);
    return true;
  }

 private:
  static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

  // One cache line per order: 40 B Order + 2 pointers = 56 B, padded to 64.
  struct alignas(64) Node {
    Order order;
    Node* prev;
    Node* next;
  };
  static_assert(sizeof(Node) == 64, "Node should be exactly one cache line");
  using Slab = SlabAllocator<Node, MaxOrders>;

  struct Level {
    Node* head = nullptr;  // oldest: matched first
    Node* tail = nullptr;  // newest
  };

  // ---- 2-level bitmap: which price levels are non-empty ----------------------
  struct Bitmap {
    static constexpr std::size_t W = NumLevels / 64;  // level words
    static constexpr std::size_t S = (W + 63) / 64;   // summary words
    std::array<std::uint64_t, W> words{};
    std::array<std::uint64_t, S> summary{};  // bit w set <=> words[w] != 0

    void set(std::size_t i) {
      words[i >> 6] |= (1ULL << (i & 63));
      summary[i >> 12] |= (1ULL << ((i >> 6) & 63));
    }
    void clear(std::size_t i) {
      const std::size_t w = i >> 6;
      words[w] &= ~(1ULL << (i & 63));
      if (words[w] == 0) summary[w >> 6] &= ~(1ULL << (w & 63));
    }
    // lowest set index >= from, or kNone
    std::size_t nextSet(std::size_t from) const {
      if (from >= NumLevels) return kNone;
      std::size_t w = from >> 6;
      std::uint64_t m = words[w] & (~0ULL << (from & 63));
      if (m) return (w << 6) + static_cast<std::size_t>(__builtin_ctzll(m));
      std::size_t nw = w + 1;
      if (nw >= W) return kNone;
      std::size_t s = nw >> 6;
      std::uint64_t sm = summary[s] & (~0ULL << (nw & 63));
      while (sm == 0) {
        if (++s >= S) return kNone;
        sm = summary[s];
      }
      const std::size_t w2 = (s << 6) + static_cast<std::size_t>(__builtin_ctzll(sm));
      return (w2 << 6) + static_cast<std::size_t>(__builtin_ctzll(words[w2]));
    }
    // highest set index <= from, or kNone
    std::size_t prevSet(std::size_t from) const {
      std::size_t w = from >> 6;
      std::uint64_t m = words[w] & (~0ULL >> (63 - (from & 63)));
      if (m) return (w << 6) + 63 - static_cast<std::size_t>(__builtin_clzll(m));
      if (w == 0) return kNone;
      std::size_t pw = w - 1;
      std::size_t s = pw >> 6;
      std::uint64_t sm = summary[s] & (~0ULL >> (63 - (pw & 63)));
      while (sm == 0) {
        if (s == 0) return kNone;
        sm = summary[--s];
      }
      const std::size_t w2 = (s << 6) + 63 - static_cast<std::size_t>(__builtin_clzll(sm));
      return (w2 << 6) + 63 - static_cast<std::size_t>(__builtin_clzll(words[w2]));
    }
  };

  // ---- flat open-addressing hash: order id -> Node* ----------------------------
  // Linear probing, load factor <= 0.5 (slots >= 2 * MaxOrders), deletion by
  // backward shift (no tombstones, no rehash, lookups stay short).
  struct HashSlot {
    OrderId key = 0;
    Node* node = nullptr;  // nullptr == empty
  };
  static constexpr std::size_t nextPow2(std::size_t v) {
    std::size_t p = 1;
    while (p < v) p <<= 1;
    return p;
  }
  static constexpr std::size_t kHashSize = nextPow2(MaxOrders * 2);
  static constexpr std::size_t kHashMask = kHashSize - 1;

  static std::size_t hashOf(OrderId id) {
    return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> 20) & kHashMask;
  }

  std::size_t findSlot(OrderId id) const {
    std::size_t i = hashOf(id);
    while (hash_[i].node != nullptr) {
      if (hash_[i].key == id) return i;
      i = (i + 1) & kHashMask;
    }
    return kNone;
  }

  void hashInsert(OrderId id, Node* n) {
    std::size_t i = hashOf(id);
    while (hash_[i].node != nullptr) i = (i + 1) & kHashMask;
    hash_[i].key = id;
    hash_[i].node = n;
  }

  void hashErase(std::size_t pos) {
    std::size_t i = pos;
    std::size_t j = pos;
    for (;;) {
      j = (j + 1) & kHashMask;
      if (hash_[j].node == nullptr) break;
      const std::size_t k = hashOf(hash_[j].key);
      // If k lies cyclically in (i, j], the entry is already in a valid
      // position; otherwise shift it back into the hole at i.
      const bool in_range = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
      if (in_range) continue;
      hash_[i] = hash_[j];
      i = j;
    }
    hash_[i].node = nullptr;
  }

  // ---- unlink a node from its level, fix best price, free memory ---------------
  void remove(Node* n, std::size_t hash_slot) {
    const std::size_t idx = static_cast<std::size_t>(n->order.price - min_price_);
    const bool is_buy = (n->order.side == Side::Buy);
    Level& lvl = (is_buy ? bids_ : asks_)[idx];

    if (n->prev) n->prev->next = n->next; else lvl.head = n->next;
    if (n->next) n->next->prev = n->prev; else lvl.tail = n->prev;

    if (lvl.head == nullptr) {  // level became empty
      if (is_buy) {
        bid_map_.clear(idx);
        --bid_levels_;
        if (idx == best_bid_) best_bid_ = bid_map_.prevSet(idx);
      } else {
        ask_map_.clear(idx);
        --ask_levels_;
        if (idx == best_ask_) best_ask_ = ask_map_.nextSet(idx);
      }
    }
    hashErase(hash_slot);
    n->~Node();
    slab_->deallocate(n);
  }

  Price min_price_;
  std::unique_ptr<Slab> slab_;
  std::vector<Level> bids_;
  std::vector<Level> asks_;
  std::vector<HashSlot> hash_;
  Bitmap bid_map_;
  Bitmap ask_map_;
  std::size_t best_bid_ = kNone;
  std::size_t best_ask_ = kNone;
  std::size_t bid_levels_ = 0;
  std::size_t ask_levels_ = 0;
};
