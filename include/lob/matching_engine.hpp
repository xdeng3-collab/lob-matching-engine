#pragma once

#include <vector>

#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

// What happened to one submission.
struct SubmitResult {
  OrderId id = kInvalidOrderId;
  Status status = Status::Rejected;
  RejectReason reason = RejectReason::None;
  Quantity filled = 0;
  Quantity resting = 0;
  std::size_t first_fill = 0;  // index into the engine's fill log
  std::size_t fill_count = 0;
};

// How to resolve an incoming order that would trade against its own resting
// order. Venues differ; the choice is explicit here so a backtest states which
// regime it assumed.
enum class SelfTradePolicy : std::uint8_t {
  Allow,          // no prevention; the participant trades with itself
  CancelResting,  // drop the resting order, keep taking
  CancelIncoming, // stop the aggressor at the point of contact
};

// Price-time-priority matching over an OrderBook.
//
// The engine owns no clock and no randomness. Every effect is a pure function
// of the event stream, so replaying the same events replays the same fills --
// which is what makes a backtest worth anything.
class MatchingEngine {
 public:
  MatchingEngine(Price min_price, Price max_price,
                 SelfTradePolicy policy = SelfTradePolicy::CancelResting)
      : book_(min_price, max_price), policy_(policy) {}

  const OrderBook& book() const noexcept { return book_; }
  OrderBook& book() noexcept { return book_; }

  const std::vector<Fill>& fills() const noexcept { return fills_; }
  void clear_fills() noexcept { fills_.clear(); }

  SubmitResult submit(const OrderRequest& request) {
    SubmitResult result;
    result.id = request.id;
    result.first_fill = fills_.size();

    if (request.quantity <= 0) {
      result.reason = RejectReason::NonPositiveQuantity;
      return result;
    }
    if (book_.find(request.id) != nullptr) {
      result.reason = RejectReason::DuplicateOrderId;
      return result;
    }
    if (request.type == OrderType::Limit) {
      if (request.price == kInvalidPrice) {
        result.reason = RejectReason::UnpricedLimitOrder;
        return result;
      }
      if (!book_.in_range(request.price)) {
        result.reason = RejectReason::PriceOutOfRange;
        return result;
      }
    }

    // A market order is a limit order at the most aggressive representable
    // price. Treating it that way keeps one matching loop instead of two.
    const Price limit = request.type == OrderType::Market
                            ? (request.side == Side::Buy ? book_.max_price()
                                                         : book_.min_price())
                            : request.price;

    if (request.tif == TimeInForce::FOK &&
        available_against(request.side, limit, request.participant) < request.quantity) {
      result.status = Status::Cancelled;
      return result;
    }

    Quantity remaining = request.quantity;
    match(request, limit, remaining);
    result.filled = request.quantity - remaining;
    result.fill_count = fills_.size() - result.first_fill;

    if (remaining == 0) {
      result.status = Status::FilledComplete;
      return result;
    }
    if (request.type == OrderType::Market) {
      result.status = result.filled > 0 ? Status::Cancelled : Status::Rejected;
      if (result.filled == 0) result.reason = RejectReason::NoLiquidityForMarketOrder;
      return result;
    }
    if (request.tif != TimeInForce::GTC) {
      result.status = Status::Cancelled;
      return result;
    }

    Order resting;
    resting.id = request.id;
    resting.participant = request.participant;
    resting.price = request.price;
    resting.remaining = remaining;
    resting.accepted_at = request.timestamp;
    resting.side = request.side;
    book_.insert(resting);

    result.status = Status::Accepted;
    result.resting = remaining;
    return result;
  }

  bool cancel(OrderId id) { return book_.cancel(id); }

  // Cancel-replace. Reducing quantity at the same price keeps time priority,
  // exactly as most venues do; any other change re-queues the order at the back
  // of its level. Modelling this honestly matters, because a simulator that
  // silently preserves priority makes every quoting strategy look profitable.
  SubmitResult replace(OrderId id, Price new_price, Quantity new_quantity,
                       Timestamp timestamp) {
    SubmitResult result;
    result.id = id;
    result.first_fill = fills_.size();

    const Order* existing = book_.find(id);
    if (existing == nullptr) {
      result.reason = RejectReason::None;
      return result;
    }
    if (new_quantity <= 0) {
      book_.cancel(id);
      result.status = Status::Cancelled;
      return result;
    }
    if (new_price == existing->price && new_quantity < existing->remaining) {
      book_.reduce(id, new_quantity);
      result.status = Status::Accepted;
      result.resting = new_quantity;
      return result;
    }

    OrderRequest request;
    request.id = id;
    request.participant = existing->participant;
    request.side = existing->side;
    request.type = OrderType::Limit;
    request.tif = TimeInForce::GTC;
    request.price = new_price;
    request.quantity = new_quantity;
    request.timestamp = timestamp;
    book_.cancel(id);
    return submit(request);
  }

 private:
  // Size resting on the opposite side that this order could legally take.
  // Used only by FOK, which must know before it touches the book.
  Quantity available_against(Side side, Price limit, ParticipantId participant) const {
    Quantity total = 0;
    Price price = side == Side::Buy ? book_.best_ask() : book_.best_bid();
    while (price != kInvalidPrice && crosses(side, limit, price)) {
      if (policy_ == SelfTradePolicy::Allow) {
        total += book_.quantity_at(price);
      } else {
        // Size this participant would match against itself is not liquidity.
        book_.for_each_at(price, [&](const Order& order) {
          if (order.participant != participant) total += order.remaining;
        });
      }
      price = side == Side::Buy ? book_.next_price_above(price)
                                : book_.next_price_below(price);
    }
    return total;
  }

  void match(const OrderRequest& request, Price limit, Quantity& remaining) {
    while (remaining > 0) {
      const Price price =
          request.side == Side::Buy ? book_.best_ask() : book_.best_bid();
      if (price == kInvalidPrice || !crosses(request.side, limit, price)) return;

      Order* resting = book_.front_at(price);
      if (resting == nullptr) return;

      if (resting->participant == request.participant &&
          policy_ != SelfTradePolicy::Allow) {
        if (policy_ == SelfTradePolicy::CancelIncoming) return;
        book_.cancel(resting->id);
        continue;
      }

      // The resting order set the price. That is the whole content of price
      // priority, and it is why a taker never improves on the touch.
      const OrderId resting_id = resting->id;
      const ParticipantId resting_participant = resting->participant;
      const Quantity taken = book_.consume_front(price, remaining);
      if (taken == 0) return;
      remaining -= taken;

      Fill fill;
      fill.aggressor_id = request.id;
      fill.resting_id = resting_id;
      fill.aggressor = request.participant;
      fill.resting_participant = resting_participant;
      fill.price = price;
      fill.quantity = taken;
      fill.timestamp = request.timestamp;
      fill.aggressor_side = request.side;
      fills_.push_back(fill);
    }
  }

  OrderBook book_;
  SelfTradePolicy policy_;
  std::vector<Fill> fills_;
};

}  // namespace lob
