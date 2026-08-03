#pragma once

#include "lob/types.hpp"

namespace lob {

// One resting order. Laid out as a trivially copyable aggregate so the book can
// hold them in a contiguous slab: the hot path walks the FIFO at a price level,
// and that walk should stay inside as few cache lines as possible.
//
// `next` / `prev` are slot indices into that slab rather than pointers. Indices
// are half the width of a pointer and survive the slab being reallocated.
struct Order {
  OrderId id = kInvalidOrderId;
  ParticipantId participant = 0;
  Price price = kInvalidPrice;
  Quantity remaining = 0;
  Timestamp accepted_at = 0;
  Side side = Side::Buy;

  std::uint32_t next = kInvalidSlot;
  std::uint32_t prev = kInvalidSlot;
};

// A submission request. Market orders ignore `price`.
struct OrderRequest {
  OrderId id = kInvalidOrderId;
  ParticipantId participant = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::GTC;
  Price price = kInvalidPrice;
  Quantity quantity = 0;
  Timestamp timestamp = 0;
};

// One execution. `aggressor_*` is the incoming order, `resting_*` the order
// that was already on the book and therefore set the price.
struct Fill {
  OrderId aggressor_id = kInvalidOrderId;
  OrderId resting_id = kInvalidOrderId;
  ParticipantId aggressor = 0;
  ParticipantId resting_participant = 0;
  Price price = kInvalidPrice;
  Quantity quantity = 0;
  Timestamp timestamp = 0;
  Side aggressor_side = Side::Buy;
};

}  // namespace lob
