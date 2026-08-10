// Scenario tests: the behaviours a venue is specified by.

#include "lob/matching_engine.hpp"
#include "microtest.hpp"

using namespace lob;

namespace {

OrderRequest limit(OrderId id, Side side, Price price, Quantity qty,
                   ParticipantId who = 1, TimeInForce tif = TimeInForce::GTC) {
  OrderRequest r;
  r.id = id;
  r.participant = who;
  r.side = side;
  r.type = OrderType::Limit;
  r.tif = tif;
  r.price = price;
  r.quantity = qty;
  return r;
}

OrderRequest market(OrderId id, Side side, Quantity qty, ParticipantId who = 1) {
  OrderRequest r;
  r.id = id;
  r.participant = who;
  r.side = side;
  r.type = OrderType::Market;
  r.tif = TimeInForce::IOC;
  r.quantity = qty;
  return r;
}

}  // namespace

TEST(resting_orders_set_the_touch) {
  MatchingEngine engine(1, 1000);
  engine.submit(limit(1, Side::Buy, 100, 10));
  engine.submit(limit(2, Side::Sell, 105, 10));
  CHECK_EQ(engine.book().best_bid(), 100);
  CHECK_EQ(engine.book().best_ask(), 105);
  CHECK_EQ(engine.book().spread(), 5);
  CHECK(engine.book().check_invariants());
}

TEST(aggressor_trades_at_the_resting_price) {
  MatchingEngine engine(1, 1000);
  engine.submit(limit(1, Side::Sell, 100, 10, 1));
  // Buyer is willing to pay 110 but the book only asks 100.
  const SubmitResult result = engine.submit(limit(2, Side::Buy, 110, 10, 2));
  CHECK_EQ(result.status, Status::FilledComplete);
  CHECK_EQ(result.filled, 10);
  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_EQ(engine.fills()[0].price, 100);
  CHECK(engine.book().check_invariants());
}

TEST(same_price_fills_in_arrival_order) {
  MatchingEngine engine(1, 1000);
  engine.submit(limit(1, Side::Sell, 100, 5, 1));
  engine.submit(limit(2, Side::Sell, 100, 5, 2));
  engine.submit(limit(3, Side::Sell, 100, 5, 3));

  engine.submit(limit(4, Side::Buy, 100, 12, 9));
  const std::vector<Fill>& fills = engine.fills();
  CHECK_EQ(fills.size(), 3u);
  CHECK_EQ(fills[0].resting_id, 1u);  // oldest first
  CHECK_EQ(fills[1].resting_id, 2u);
  CHECK_EQ(fills[2].resting_id, 3u);
  CHECK_EQ(fills[2].quantity, 2);     // partial fill of the last one
  CHECK_EQ(engine.book().quantity_at(100), 3);
  CHECK(engine.book().check_invariants());
}

TEST(taker_sweeps_multiple_levels_in_price_order) {
  MatchingEngine engine(1, 1000);
  engine.submit(limit(1, Side::Sell, 100, 5, 1));
  engine.submit(limit(2, Side::Sell, 101, 5, 1));
  engine.submit(limit(3, Side::Sell, 102, 5, 1));

  const SubmitResult result = engine.submit(limit(4, Side::Buy, 101, 20, 2));
  CHECK_EQ(result.filled, 10);                 // 102 is outside the limit
  CHECK_EQ(result.resting, 10);                // remainder rests as a bid
  CHECK_EQ(engine.fills()[0].price, 100);      // cheapest first
  CHECK_EQ(engine.fills()[1].price, 101);
  CHECK_EQ(engine.book().best_bid(), 101);
  CHECK_EQ(engine.book().best_ask(), 102);
  CHECK(engine.book().check_invariants());
}

TEST(ioc_cancels_its_remainder) {
  MatchingEngine engine(1, 1000);
  engine.submit(limit(1, Side::Sell, 100, 3, 1));
  const SubmitResult result =
      engine.submit(limit(2, Side::Buy, 100, 10, 2, TimeInForce::IOC));
  CHECK_EQ(result.status, Status::Cancelled);
  CHECK_EQ(result.filled, 3);
  CHECK_EQ(engine.book().resting_orders(), 0u);
}

TEST(fok_is_all_or_nothing_and_leaves_no_trace) {
  MatchingEngine engine(1, 1000);
  engine.submit(limit(1, Side::Sell, 100, 3, 1));

  const SubmitResult short_fill =
      engine.submit(limit(2, Side::Buy, 100, 10, 2, TimeInForce::FOK));
  CHECK_EQ(short_fill.status, Status::Cancelled);
  CHECK_EQ(short_fill.filled, 0);
  CHECK_EQ(engine.fills().size(), 0u);          // the book was never touched
  CHECK_EQ(engine.book().quantity_at(100), 3);

  const SubmitResult exact =
      engine.submit(limit(3, Side::Buy, 100, 3, 2, TimeInForce::FOK));
  CHECK_EQ(exact.status, Status::FilledComplete);
  CHECK(engine.book().check_invariants());
}

TEST(market_order_takes_every_reachable_level) {
  MatchingEngine engine(1, 1000);
  engine.submit(limit(1, Side::Sell, 100, 5, 1));
  engine.submit(limit(2, Side::Sell, 300, 5, 1));
  const SubmitResult result = engine.submit(market(3, Side::Buy, 8, 2));
  CHECK_EQ(result.filled, 8);
  CHECK_EQ(engine.fills()[1].price, 300);  // pays right through the gap
  CHECK(engine.book().check_invariants());
}

TEST(self_trade_prevention_drops_the_resting_side) {
  MatchingEngine engine(1, 1000, SelfTradePolicy::CancelResting);
  engine.submit(limit(1, Side::Sell, 100, 5, /*who=*/7));
  engine.submit(limit(2, Side::Sell, 100, 5, /*who=*/8));

  const SubmitResult result = engine.submit(limit(3, Side::Buy, 100, 5, /*who=*/7));
  CHECK_EQ(result.filled, 5);
  CHECK_EQ(engine.fills().size(), 1u);
  CHECK_EQ(engine.fills()[0].resting_id, 2u);   // matched the other participant
  CHECK(engine.book().find(1) == nullptr);      // own order was cancelled
  CHECK(engine.book().check_invariants());
}

TEST(cancel_removes_the_order_and_steps_the_touch_back) {
  MatchingEngine engine(1, 1000);
  engine.submit(limit(1, Side::Buy, 100, 5));
  engine.submit(limit(2, Side::Buy, 99, 5));
  CHECK_EQ(engine.book().best_bid(), 100);
  CHECK(engine.cancel(1));
  CHECK_EQ(engine.book().best_bid(), 99);
  CHECK(!engine.cancel(1));                     // cancelling twice is a no-op
  CHECK(engine.book().check_invariants());
}

TEST(reducing_in_place_keeps_time_priority) {
  MatchingEngine engine(1, 1000);
  engine.submit(limit(1, Side::Buy, 100, 10, 1));
  engine.submit(limit(2, Side::Buy, 100, 10, 2));
  CHECK_EQ(engine.book().queue_position(2), 1u);

  engine.replace(1, 100, 4, /*timestamp=*/0);
  CHECK_EQ(engine.book().quantity_at(100), 14);
  CHECK_EQ(engine.book().queue_position(2), 1u);  // order 1 still ahead
  CHECK(engine.book().check_invariants());
}

TEST(queue_ahead_quantity_is_what_a_backtest_must_wait_for) {
  MatchingEngine engine(1, 1000);
  engine.submit(limit(1, Side::Buy, 100, 7, 1));
  engine.submit(limit(2, Side::Buy, 100, 3, 2));
  engine.submit(limit(3, Side::Buy, 100, 5, 3));
  CHECK_EQ(engine.book().queue_ahead_quantity(3), 10);
  CHECK_EQ(engine.book().queue_ahead_quantity(1), 0);
}

TEST(malformed_submissions_are_rejected) {
  MatchingEngine engine(1, 1000);
  CHECK_EQ(engine.submit(limit(1, Side::Buy, 100, 0)).reason,
           RejectReason::NonPositiveQuantity);
  CHECK_EQ(engine.submit(limit(2, Side::Buy, 5000, 10)).reason,
           RejectReason::PriceOutOfRange);
  engine.submit(limit(3, Side::Buy, 100, 10));
  CHECK_EQ(engine.submit(limit(3, Side::Buy, 100, 10)).reason,
           RejectReason::DuplicateOrderId);
}

int main() { return microtest::run_all(); }
