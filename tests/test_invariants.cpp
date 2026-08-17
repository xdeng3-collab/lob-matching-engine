// Property-based test.
//
// Scenario tests check the cases someone thought of. This one generates long
// random order flows and asserts, after every single operation, the properties
// that must hold no matter what arrived:
//
//   1. the book is never crossed (best bid < best ask);
//   2. every level's cached aggregate equals the sum of its FIFO;
//   3. the FIFO's forward and backward links agree, and the occupancy bitmap
//      agrees with the levels;
//   4. quantity is conserved: filled + resting + cancelled == submitted;
//   5. every fill price lies between the two orders' limits.
//
// A seed is printed on failure so any counterexample replays exactly.

#include <cstdio>
#include <cstdlib>
#include <random>

#include "lob/matching_engine.hpp"
#include "microtest.hpp"

using namespace lob;

namespace {

constexpr Price kMinPrice = 900;
constexpr Price kMaxPrice = 1100;

struct Ledger {
  Quantity submitted = 0;
  Quantity filled = 0;      // counted once per side, so fills count twice
  Quantity cancelled = 0;
};

// Runs one randomised session and returns false at the first violated property.
bool run_session(std::uint64_t seed, int operations) {
  std::mt19937_64 rng(seed);
  MatchingEngine engine(kMinPrice, kMaxPrice, SelfTradePolicy::Allow);

  std::uniform_int_distribution<int> action(0, 99);
  std::uniform_int_distribution<Price> price(kMinPrice, kMaxPrice);
  std::uniform_int_distribution<Quantity> size(1, 50);
  std::uniform_int_distribution<int> coin(0, 1);
  std::uniform_int_distribution<int> who(1, 4);

  std::vector<OrderId> live;
  Ledger ledger;
  OrderId next_id = 1;

  for (int step = 0; step < operations; ++step) {
    const int roll = action(rng);

    if (roll < 20 && !live.empty()) {
      // Cancel a random live order.
      std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
      const std::size_t slot = pick(rng);
      const OrderId id = live[slot];
      if (const Order* order = engine.book().find(id)) {
        ledger.cancelled += order->remaining;
        engine.cancel(id);
      }
      live[slot] = live.back();
      live.pop_back();
    } else {
      OrderRequest request;
      request.id = next_id++;
      request.participant = static_cast<ParticipantId>(who(rng));
      request.side = coin(rng) ? Side::Buy : Side::Sell;
      request.quantity = size(rng);
      request.timestamp = step;

      if (roll >= 95) {
        request.type = OrderType::Market;
        request.tif = TimeInForce::IOC;
      } else {
        request.type = OrderType::Limit;
        request.price = price(rng);
        request.tif = roll >= 88 ? TimeInForce::IOC
                                 : (roll >= 84 ? TimeInForce::FOK : TimeInForce::GTC);
      }

      const std::size_t fills_before = engine.fills().size();
      const SubmitResult result = engine.submit(request);
      if (result.status == Status::Rejected) {
        // A rejection never reaches the book, so it enters no side of the
        // ledger. (Getting this wrong is what the ledger caught the first time
        // this test ran: rejected market orders were being counted as
        // cancellations without ever having been counted as submissions.)
        continue;
      }
      ledger.submitted += request.quantity;

      // Property 5: every fill is inside both orders' limits.
      for (std::size_t i = fills_before; i < engine.fills().size(); ++i) {
        const Fill& fill = engine.fills()[i];
        if (fill.quantity <= 0) return false;
        if (request.type == OrderType::Limit &&
            !crosses(request.side, request.price, fill.price)) {
          return false;
        }
        ledger.filled += fill.quantity;
      }

      ledger.cancelled += request.quantity - result.filled - result.resting;
      if (result.status == Status::Accepted) live.push_back(request.id);
    }

    // Properties 1 through 3, after every operation rather than at the end.
    if (!engine.book().check_invariants()) return false;
  }

  // Property 4. `filled` counts the aggressor's side; the resting side removed
  // the same quantity, so submitted == filled(aggressor) + filled(resting)
  // + still resting + cancelled.
  Quantity resting = 0;
  for (Price p = kMinPrice; p <= kMaxPrice; ++p) resting += engine.book().quantity_at(p);
  return ledger.submitted == ledger.filled * 2 + resting + ledger.cancelled;
}

}  // namespace

TEST(random_order_flow_preserves_book_invariants) {
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    if (!run_session(seed, 500)) {
      std::printf("    counterexample: seed=%llu\n",
                  static_cast<unsigned long long>(seed));
      CHECK(false);
      return;
    }
  }
}

TEST(long_session_stays_consistent) {
  CHECK(run_session(/*seed=*/20260913, /*operations=*/50000));
}

TEST(book_survives_being_emptied_and_refilled) {
  // Repeatedly draining the book exercises the touch-recovery scan, which is
  // where an occupancy bitmap is easiest to get wrong.
  MatchingEngine engine(kMinPrice, kMaxPrice);
  OrderId id = 1;
  for (int round = 0; round < 50; ++round) {
    for (Price p = 1000; p < 1010; ++p) {
      OrderRequest r;
      r.id = id++;
      r.side = Side::Sell;
      r.type = OrderType::Limit;
      r.price = p;
      r.quantity = 5;
      engine.submit(r);
    }
    CHECK_EQ(engine.book().best_ask(), 1000);

    OrderRequest sweep;
    sweep.id = id++;
    sweep.participant = 99;
    sweep.side = Side::Buy;
    sweep.type = OrderType::Market;
    sweep.tif = TimeInForce::IOC;
    sweep.quantity = 50;
    engine.submit(sweep);

    CHECK_EQ(engine.book().best_ask(), kInvalidPrice);
    CHECK_EQ(engine.book().resting_orders(), 0u);
    CHECK(engine.book().check_invariants());
  }
}

int main() { return microtest::run_all(); }
