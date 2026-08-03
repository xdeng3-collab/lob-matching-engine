#pragma once

#include <cstdint>
#include <limits>

namespace lob {

// Prices are integer ticks, never floating point. Floating-point comparison in
// a matching engine produces orders that almost cross, which is the single
// most expensive class of bug an exchange simulator can have.
using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::uint64_t;
using ParticipantId = std::uint32_t;

// Nanoseconds since an arbitrary epoch. The engine never reads a clock itself:
// time arrives with the event, so a replay is bit-for-bit reproducible.
using Timestamp = std::int64_t;

inline constexpr Price kInvalidPrice = std::numeric_limits<Price>::min();
inline constexpr OrderId kInvalidOrderId = 0;
inline constexpr std::uint32_t kInvalidSlot = std::numeric_limits<std::uint32_t>::max();

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

// True when `price` is at least as aggressive as `limit` for a resting order on
// `side`. Buy orders match downward, sell orders match upward.
constexpr bool crosses(Side side, Price limit, Price price) noexcept {
  return side == Side::Buy ? price <= limit : price >= limit;
}

enum class OrderType : std::uint8_t { Limit, Market };

enum class TimeInForce : std::uint8_t {
  GTC,  // rest on the book until cancelled
  IOC,  // take what is available, cancel the remainder
  FOK,  // fill in full or do nothing at all
};

// What the engine did with a submission.
enum class Status : std::uint8_t {
  Accepted,       // resting on the book (possibly after a partial fill)
  FilledComplete, // fully filled, nothing rests
  Cancelled,      // IOC remainder cancelled, or FOK that could not fill
  Rejected,       // malformed, or a market order with no liquidity
};

// Why a submission was rejected. Kept separate from Status so the caller can
// log a reason without parsing strings.
enum class RejectReason : std::uint8_t {
  None,
  NonPositiveQuantity,
  PriceOutOfRange,
  UnpricedLimitOrder,
  NoLiquidityForMarketOrder,
  DuplicateOrderId,
};

}  // namespace lob
