# lob-matching-engine

A price-time-priority limit order book and matching engine in C++17, built to be
correct under adversarial order flow and predictable in the tail.

Header-only, no third-party dependencies, builds and tests in under a second.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bench_book
```

## What it does

* Limit and market orders, GTC / IOC / FOK, cancel and cancel-replace
* Price-time priority: the resting order sets the trade price, and same-priced
  orders fill in arrival order
* Partial fills, multi-level sweeps, and configurable self-trade prevention
* Queue-position accounting — the number every backtest needs and most omit

## Measured

Two million synthetic events (30% cancels, quotes clustered near the mid over a
200,000-tick grid), replayed against both implementations from the same seed.
Apple M-series, clang 17, `-O2`:

| Level container | p50 | p99 | p99.9 | Throughput |
|---|---|---|---|---|
| **Flat tick-indexed array** | **42 ns** | **541 ns** | **1.4 µs** | **8.3 M ops/s** |
| `std::map<Price, Level>` | 125 ns | 9.3 µs | 16.8 µs | 0.9 M ops/s |

Roughly 3× at the median, **17× at p99**, and 9× the throughput.

Two honest caveats, because a benchmark without them is marketing:

* The `std::map` baseline only inserts and cancels. It never matches, so it is
  doing strictly *less* work than the flat book it loses to.
* Per-operation timing includes `steady_clock` overhead, tens of nanoseconds on
  this machine. That inflates the p50 column for both rows and compresses the
  ratio; the tail columns are where the difference actually lives.

Percentiles rather than means is the whole point. A path that is 40 ns typically
and 9 µs at p99.9 is a different system from one that is 120 ns flat, and only
the second is something you can build on.

## Why the container choice is the design

A `std::map` of price levels walks a red-black tree on every book operation and
scatters its nodes across the heap. Prices are already discrete — they are
integer ticks — so the map is paying for generality it does not need.

This book stores levels in a flat `std::vector` indexed by `price - min_price`.
Every level, including the touch, is one O(1) offset away. The cost is that the
price range is declared up front, which is the same trade a real venue makes
with its price banding rules.

Three details make it hold up:

* **Intrusive FIFO per level.** Each level is a doubly linked list of order
  slots, so time priority is the natural order of the list and cancelling is
  O(1) once the slot is known. The slots are indices into a contiguous slab, not
  pointers: half the width, and they survive reallocation.

* **Occupancy bitmap.** Finding the next best price after a level empties is a
  word-at-a-time scan with `__builtin_ctzll`, not a per-tick loop. A book with a
  wide spread and few levels stays fast instead of degrading with the grid size.

* **Integer prices, everywhere.** Never `double`. Floating-point comparison in a
  matching engine produces orders that *almost* cross, which is the most
  expensive class of bug this kind of system can have.

## Correctness

Fourteen scenario tests cover the behaviours a venue is specified by: the
resting order sets the price, same-price orders fill in arrival order, sweeps
take cheapest-first, IOC cancels its remainder, FOK leaves no trace when it
cannot fill, market orders with an empty book are rejected, self-trade
prevention drops the resting side.

The more useful suite is property-based. It generates long random order flows —
limit, market, IOC, FOK, cancels, four participants — and after **every single
operation** asserts:

1. the book is never crossed;
2. each level's cached aggregate equals the sum of its FIFO;
3. the forward and backward links agree, and the occupancy bitmap agrees with
   the levels;
4. quantity is conserved: submitted == filled + resting + cancelled;
5. every fill price lies inside both orders' limits.

200 seeds × 500 operations, plus one 50,000-operation session, plus a drain-and-
refill loop that specifically exercises touch recovery — the place an occupancy
bitmap is easiest to get wrong. A failing seed is printed so any counterexample
replays exactly.

The ledger property earned its keep on the first run: it caught rejected market
orders being counted as cancellations without ever having been counted as
submissions. That was a bug in the test rather than the engine, which is the
ordinary outcome and still the reason to write it.

## What this deliberately does not do

Being explicit about the boundary matters more than the feature list:

* **Cancel-replace forfeits time priority** unless it is a pure size reduction
  at the same price. Simulators that silently preserve priority make every
  quoting strategy look profitable.
* **No market impact model.** Orders do not move the book beyond the liquidity
  they consume. Backtests built on this will be optimistic for any size that a
  real venue would notice.
* **No fees, rebates, or latency.** Market-making PnL frequently lives entirely
  inside the fee tier, so a strategy result from this engine alone is not a
  strategy result.
* **No clock and no randomness inside the engine.** Time arrives with the event.
  That is what makes a replay reproducible, and it is a constraint, not an
  omission.

The engine is the substrate. The event-driven backtester with queue-position
fills and a configurable latency model is the next layer, and is where those
last three items get answered.

## Layout

```
include/lob/
  types.hpp            prices, sides, time-in-force, statuses
  order.hpp            resting order, submission request, fill
  order_book.hpp       flat levels, intrusive FIFOs, occupancy bitmap
  matching_engine.hpp  crossing logic, TIF handling, self-trade policy
tests/
  microtest.hpp        ~60-line harness, so the suite builds offline
  test_matching.cpp    scenario tests
  test_invariants.cpp  property-based tests
bench/
  bench_book.cpp       percentile latency, flat book vs std::map
```

## License

MIT
