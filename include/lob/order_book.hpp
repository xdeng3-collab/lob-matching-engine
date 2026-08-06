#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "lob/order.hpp"
#include "lob/types.hpp"

namespace lob {

// A price-time-priority limit order book over a bounded, discrete price grid.
//
// Design notes, because the container choice is the whole point of this class:
//
//   * Price levels live in a flat vector indexed by (price - min_price). Access
//     to any level, including the best one, is O(1) and touches one cache line.
//     The usual `std::map<Price, Level>` walks a red-black tree on every single
//     book operation, and the nodes are scattered across the heap. The cost is
//     that the price range must be declared up front, which is exactly the
//     tradeoff a real venue makes with its price banding rules.
//
//   * Each level is an intrusive FIFO of order slots, so time priority is the
//     natural order of the list and cancelling is O(1) once the slot is known.
//
//   * An occupancy bitmap tracks which levels are non-empty. Finding the next
//     best price after a level is exhausted is a word-at-a-time scan with a
//     count-trailing-zeros intrinsic rather than a per-tick loop, which keeps
//     sparse books (wide spreads, few levels) from degrading.
class OrderBook {
 public:
  struct Level {
    std::uint32_t head = kInvalidSlot;  // front of the FIFO: oldest order
    std::uint32_t tail = kInvalidSlot;  // back of the FIFO: newest order
    Quantity quantity = 0;              // aggregate resting size
    std::uint32_t count = 0;            // number of resting orders
  };

  // `min_price` and `max_price` are inclusive tick bounds.
  OrderBook(Price min_price, Price max_price)
      : min_price_(min_price),
        max_price_(max_price),
        levels_(static_cast<std::size_t>(max_price - min_price + 1)),
        occupancy_(word_count(levels_.size()), 0ULL) {
    assert(max_price >= min_price);
  }

  bool in_range(Price price) const noexcept {
    return price >= min_price_ && price <= max_price_;
  }

  Price min_price() const noexcept { return min_price_; }
  Price max_price() const noexcept { return max_price_; }

  // kInvalidPrice when that side is empty.
  Price best_bid() const noexcept { return best_bid_; }
  Price best_ask() const noexcept { return best_ask_; }

  // Only meaningful when both sides are populated.
  Price spread() const noexcept {
    if (best_bid_ == kInvalidPrice || best_ask_ == kInvalidPrice) return kInvalidPrice;
    return best_ask_ - best_bid_;
  }

  Quantity quantity_at(Price price) const noexcept {
    if (!in_range(price)) return 0;
    return levels_[index_of(price)].quantity;
  }

  std::uint32_t order_count_at(Price price) const noexcept {
    if (!in_range(price)) return 0;
    return levels_[index_of(price)].count;
  }

  std::size_t resting_orders() const noexcept { return live_orders_; }

  const Order* find(OrderId id) const noexcept {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &slab_[it->second];
  }

  // Appends a resting order to the back of its price level. The caller has
  // already decided that it does not cross.
  void insert(const Order& order) {
    assert(in_range(order.price));
    assert(order.remaining > 0);
    assert(index_.find(order.id) == index_.end());

    const std::uint32_t slot = allocate();
    Order& stored = slab_[slot];
    stored = order;
    stored.next = kInvalidSlot;
    stored.prev = kInvalidSlot;

    Level& level = levels_[index_of(order.price)];
    if (level.tail == kInvalidSlot) {
      level.head = level.tail = slot;
    } else {
      slab_[level.tail].next = slot;
      stored.prev = level.tail;
      level.tail = slot;
    }
    level.quantity += order.remaining;
    level.count += 1;

    index_.emplace(order.id, slot);
    live_orders_ += 1;
    mark_occupied(index_of(order.price));
    widen_best(order.side, order.price);
  }

  bool cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    unlink(it->second);
    index_.erase(it);
    return true;
  }

  // Reduces a resting order in place. Growing an order or moving its price must
  // go through cancel + insert, because either one forfeits time priority and
  // pretending otherwise would make the simulator optimistic.
  bool reduce(OrderId id, Quantity new_quantity) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Order& order = slab_[it->second];
    if (new_quantity <= 0) {
      unlink(it->second);
      index_.erase(it);
      return true;
    }
    if (new_quantity >= order.remaining) return false;  // not a reduction
    levels_[index_of(order.price)].quantity -= (order.remaining - new_quantity);
    order.remaining = new_quantity;
    return true;
  }

  // Front of the FIFO at `price`, or nullptr when the level is empty.
  Order* front_at(Price price) noexcept {
    if (!in_range(price)) return nullptr;
    const std::uint32_t slot = levels_[index_of(price)].head;
    return slot == kInvalidSlot ? nullptr : &slab_[slot];
  }

  // Consumes `quantity` from the order at the front of `price`, removing it once
  // it is exhausted. Returns how much was actually taken.
  Quantity consume_front(Price price, Quantity quantity) {
    Level& level = levels_[index_of(price)];
    const std::uint32_t slot = level.head;
    if (slot == kInvalidSlot || quantity <= 0) return 0;

    Order& order = slab_[slot];
    const Quantity taken = quantity < order.remaining ? quantity : order.remaining;
    order.remaining -= taken;
    level.quantity -= taken;
    if (order.remaining == 0) {
      const OrderId id = order.id;
      unlink(slot);
      index_.erase(id);
    }
    return taken;
  }

  // Visits every resting order at `price` in time priority. Gives callers a
  // way to inspect a level without exposing the slab or the FIFO links.
  template <typename Visitor>
  void for_each_at(Price price, Visitor&& visit) const {
    if (!in_range(price)) return;
    for (std::uint32_t slot = levels_[index_of(price)].head; slot != kInvalidSlot;
         slot = slab_[slot].next) {
      visit(slab_[slot]);
    }
  }

  // Nearest occupied level strictly above / below `price`, or kInvalidPrice.
  // Both are bitmap scans, not per-tick loops.
  Price next_price_above(Price price) const noexcept {
    if (price >= max_price_) return kInvalidPrice;
    const std::size_t found = scan_up(index_of(price) + 1);
    return found == kNotFound ? kInvalidPrice : price_of(found);
  }

  Price next_price_below(Price price) const noexcept {
    if (price <= min_price_) return kInvalidPrice;
    const std::size_t found = scan_down(index_of(price) - 1);
    return found == kNotFound ? kInvalidPrice : price_of(found);
  }

  // How many orders rest ahead of `id` at its own price level. This is the
  // number the backtester needs: an order only trades once everything in front
  // of it has traded or cancelled.
  std::uint32_t queue_position(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return 0;
    std::uint32_t ahead = 0;
    for (std::uint32_t slot = slab_[it->second].prev; slot != kInvalidSlot;
         slot = slab_[slot].prev) {
      ahead += 1;
    }
    return ahead;
  }

  // Aggregate resting size ahead of `id` in its level's FIFO.
  Quantity queue_ahead_quantity(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return 0;
    Quantity ahead = 0;
    for (std::uint32_t slot = slab_[it->second].prev; slot != kInvalidSlot;
         slot = slab_[slot].prev) {
      ahead += slab_[slot].remaining;
    }
    return ahead;
  }

  // Invariants any well-formed book must satisfy. Exposed so the property-based
  // test can assert them after every random operation rather than only at the
  // end of a scripted scenario.
  bool check_invariants() const {
    if (best_bid_ != kInvalidPrice && best_ask_ != kInvalidPrice &&
        best_bid_ >= best_ask_) {
      return false;  // the book is crossed; matching should have prevented it
    }
    std::size_t counted = 0;
    for (std::size_t i = 0; i < levels_.size(); ++i) {
      const Level& level = levels_[i];
      const bool occupied = is_occupied(i);
      if ((level.count == 0) != (level.head == kInvalidSlot)) return false;
      if (occupied != (level.count != 0)) return false;

      Quantity sum = 0;
      std::uint32_t seen = 0;
      std::uint32_t prev = kInvalidSlot;
      for (std::uint32_t slot = level.head; slot != kInvalidSlot;
           slot = slab_[slot].next) {
        if (slab_[slot].prev != prev) return false;   // FIFO links disagree
        if (slab_[slot].remaining <= 0) return false; // exhausted order still linked
        sum += slab_[slot].remaining;
        seen += 1;
        prev = slot;
      }
      if (prev != level.tail) return false;
      if (sum != level.quantity || seen != level.count) return false;
      counted += seen;
    }
    if (counted != live_orders_ || counted != index_.size()) return false;
    return best_bid_ == recompute_best(Side::Buy) &&
           best_ask_ == recompute_best(Side::Sell);
  }

 private:
  static std::size_t word_count(std::size_t bits) { return (bits + 63) / 64; }

  std::size_t index_of(Price price) const noexcept {
    return static_cast<std::size_t>(price - min_price_);
  }

  Price price_of(std::size_t index) const noexcept {
    return min_price_ + static_cast<Price>(index);
  }

  bool is_occupied(std::size_t index) const noexcept {
    return (occupancy_[index >> 6] >> (index & 63)) & 1ULL;
  }

  void mark_occupied(std::size_t index) noexcept {
    occupancy_[index >> 6] |= (1ULL << (index & 63));
  }

  void mark_empty(std::size_t index) noexcept {
    occupancy_[index >> 6] &= ~(1ULL << (index & 63));
  }

  // Word-at-a-time scans. `scan_down` finds the highest occupied level at or
  // below `from`; `scan_up` the lowest at or above.
  std::size_t scan_down(std::size_t from) const noexcept {
    std::size_t word = from >> 6;
    std::uint64_t bits = occupancy_[word] & (~0ULL >> (63 - (from & 63)));
    while (true) {
      if (bits) return (word << 6) + (63 - static_cast<std::size_t>(__builtin_clzll(bits)));
      if (word == 0) return kNotFound;
      --word;
      bits = occupancy_[word];
    }
  }

  std::size_t scan_up(std::size_t from) const noexcept {
    std::size_t word = from >> 6;
    std::uint64_t bits = occupancy_[word] & (~0ULL << (from & 63));
    while (true) {
      if (bits) return (word << 6) + static_cast<std::size_t>(__builtin_ctzll(bits));
      ++word;
      if (word >= occupancy_.size()) return kNotFound;
      bits = occupancy_[word];
    }
  }

  // A new resting order can only improve the touch, never worsen it.
  void widen_best(Side side, Price price) noexcept {
    if (side == Side::Buy) {
      if (best_bid_ == kInvalidPrice || price > best_bid_) best_bid_ = price;
    } else {
      if (best_ask_ == kInvalidPrice || price < best_ask_) best_ask_ = price;
    }
  }

  // Called after a level empties: the touch may need to step away.
  void narrow_best(Side side, Price price) noexcept {
    if (side == Side::Buy) {
      if (price != best_bid_) return;
      const std::size_t idx = index_of(price);
      const std::size_t found = idx == 0 ? kNotFound : scan_down(idx - 1);
      best_bid_ = found == kNotFound ? kInvalidPrice : price_of(found);
    } else {
      if (price != best_ask_) return;
      const std::size_t idx = index_of(price);
      const std::size_t found =
          idx + 1 >= levels_.size() ? kNotFound : scan_up(idx + 1);
      best_ask_ = found == kNotFound ? kInvalidPrice : price_of(found);
    }
  }

  Price recompute_best(Side side) const noexcept {
    for (std::size_t i = 0; i < levels_.size(); ++i) {
      const std::size_t idx = side == Side::Buy ? levels_.size() - 1 - i : i;
      if (levels_[idx].count == 0) continue;
      // A level holds only one side at a time: the matcher never lets the book
      // cross, so a resting bid and a resting ask cannot share a price.
      if (slab_[levels_[idx].head].side == side) return price_of(idx);
    }
    return kInvalidPrice;
  }

  std::uint32_t allocate() {
    if (free_head_ != kInvalidSlot) {
      const std::uint32_t slot = free_head_;
      free_head_ = slab_[slot].next;
      return slot;
    }
    slab_.emplace_back();
    return static_cast<std::uint32_t>(slab_.size() - 1);
  }

  void unlink(std::uint32_t slot) {
    Order& order = slab_[slot];
    Level& level = levels_[index_of(order.price)];

    if (order.prev != kInvalidSlot) slab_[order.prev].next = order.next;
    else level.head = order.next;
    if (order.next != kInvalidSlot) slab_[order.next].prev = order.prev;
    else level.tail = order.prev;

    level.quantity -= order.remaining;
    level.count -= 1;
    live_orders_ -= 1;

    const Side side = order.side;
    const Price price = order.price;
    if (level.count == 0) {
      level.quantity = 0;
      mark_empty(index_of(price));
      narrow_best(side, price);
    }

    order = Order{};
    order.next = free_head_;
    free_head_ = slot;
  }

  static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

  Price min_price_;
  Price max_price_;
  std::vector<Level> levels_;
  std::vector<std::uint64_t> occupancy_;
  std::vector<Order> slab_;
  std::unordered_map<OrderId, std::uint32_t> index_;
  std::uint32_t free_head_ = kInvalidSlot;
  std::size_t live_orders_ = 0;
  Price best_bid_ = kInvalidPrice;
  Price best_ask_ = kInvalidPrice;
};

}  // namespace lob
