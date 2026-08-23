// Latency benchmark.
//
// Reports percentiles, not means. In this domain the mean is close to useless:
// a matching path that is 40 ns typically and 9 us at p99.9 is a different
// system from one that is 120 ns flat, and only the second one is predictable.
//
// The comparison is deliberately like-for-like: the same synthetic order flow,
// the same seed, replayed against the flat tick-indexed book and against a
// std::map-of-levels book, so the only variable is the price-level container.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "lob/matching_engine.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

constexpr Price kMinPrice = 1;
constexpr Price kMaxPrice = 200000;
constexpr Price kMid = 100000;

struct Sample {
  std::vector<double> nanos;

  void add(double value) { nanos.push_back(value); }

  double percentile(double p) {
    if (nanos.empty()) return 0.0;
    std::sort(nanos.begin(), nanos.end());
    const std::size_t index = static_cast<std::size_t>(p * (nanos.size() - 1));
    return nanos[index];
  }
};

struct Event {
  bool cancel;
  OrderRequest request;
  OrderId cancel_id;
};

// One reproducible order flow, generated once and replayed against every
// implementation so the benchmark compares containers rather than inputs.
std::vector<Event> make_flow(std::size_t count, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> offset(0.0, 25.0);
  std::uniform_int_distribution<Quantity> size(1, 100);
  std::uniform_int_distribution<int> action(0, 99);
  std::uniform_int_distribution<int> coin(0, 1);

  std::vector<Event> events;
  events.reserve(count);
  std::vector<OrderId> live;
  OrderId next_id = 1;

  for (std::size_t i = 0; i < count; ++i) {
    Event event{};
    if (action(rng) < 30 && !live.empty()) {
      std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
      const std::size_t slot = pick(rng);
      event.cancel = true;
      event.cancel_id = live[slot];
      live[slot] = live.back();
      live.pop_back();
    } else {
      const Side side = coin(rng) ? Side::Buy : Side::Sell;
      // Quotes cluster near the mid, which is what makes the flat array's
      // locality pay off and what a real book looks like.
      Price price = kMid + static_cast<Price>(offset(rng));
      price += (side == Side::Buy ? -1 : 1);
      price = std::max(kMinPrice, std::min(kMaxPrice, price));

      event.cancel = false;
      event.request.id = next_id;
      event.request.side = side;
      event.request.type = OrderType::Limit;
      event.request.tif = TimeInForce::GTC;
      event.request.price = price;
      event.request.quantity = size(rng);
      event.request.timestamp = static_cast<Timestamp>(i);
      live.push_back(next_id);
      next_id += 1;
    }
    events.push_back(event);
  }
  return events;
}

// The baseline this project exists to beat: levels in an ordered map, which is
// the obvious first implementation and the one most tutorials show.
class MapBook {
 public:
  void insert(const Order& order) {
    auto& level = order.side == Side::Buy ? bids_[order.price] : asks_[order.price];
    level.push_back(order);
    index_[order.id] = {order.price, order.side};
  }

  bool cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    auto& side_map = it->second.second == Side::Buy ? bids_ : asks_;
    auto level_it = side_map.find(it->second.first);
    if (level_it != side_map.end()) {
      auto& orders = level_it->second;
      for (auto order_it = orders.begin(); order_it != orders.end(); ++order_it) {
        if (order_it->id == id) {
          orders.erase(order_it);
          break;
        }
      }
      if (orders.empty()) side_map.erase(level_it);
    }
    index_.erase(it);
    return true;
  }

  Price best_bid() const { return bids_.empty() ? kInvalidPrice : bids_.rbegin()->first; }
  Price best_ask() const { return asks_.empty() ? kInvalidPrice : asks_.begin()->first; }

 private:
  std::map<Price, std::vector<Order>> bids_;
  std::map<Price, std::vector<Order>> asks_;
  std::unordered_map<OrderId, std::pair<Price, Side>> index_;
};

void report(const char* label, Sample& sample, double seconds, std::size_t ops) {
  std::printf("%-28s p50 %8.1f  p99 %9.1f  p99.9 %10.1f ns   %7.2f M ops/s\n",
              label, sample.percentile(0.50), sample.percentile(0.99),
              sample.percentile(0.999), ops / seconds / 1e6);
}

}  // namespace

int main() {
  constexpr std::size_t kOps = 2000000;
  const std::vector<Event> flow = make_flow(kOps, /*seed=*/20260913);
  std::printf("replaying %zu events, price grid %lld ticks\n\n", flow.size(),
              static_cast<long long>(kMaxPrice - kMinPrice + 1));

  {
    MatchingEngine engine(kMinPrice, kMaxPrice, SelfTradePolicy::Allow);
    Sample sample;
    sample.nanos.reserve(kOps);
    const auto started = Clock::now();
    for (const Event& event : flow) {
      const auto t0 = Clock::now();
      if (event.cancel) {
        engine.cancel(event.cancel_id);
      } else {
        engine.submit(event.request);
      }
      const auto t1 = Clock::now();
      sample.add(std::chrono::duration<double, std::nano>(t1 - t0).count());
      if (engine.fills().size() > 4096) engine.clear_fills();
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    report("flat tick-indexed book", sample, seconds, flow.size());
  }

  {
    MapBook book;
    Sample sample;
    sample.nanos.reserve(kOps);
    const auto started = Clock::now();
    for (const Event& event : flow) {
      const auto t0 = Clock::now();
      if (event.cancel) {
        book.cancel(event.cancel_id);
      } else {
        Order order;
        order.id = event.request.id;
        order.price = event.request.price;
        order.remaining = event.request.quantity;
        order.side = event.request.side;
        book.insert(order);
      }
      const auto t1 = Clock::now();
      sample.add(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    report("std::map level book", sample, seconds, flow.size());
  }

  std::printf(
      "\nNote: the map baseline only inserts and cancels -- it never matches --\n"
      "so it is being flattered. The flat book is doing strictly more work.\n");
  return 0;
}
