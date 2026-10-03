#pragma once
// The interface every book version implements. Matching rules (all versions
// must produce exactly the same event sequence as v0):
//  - price priority, then time priority (FIFO within a level)
//  - trade price = resting (maker) order's price
//  - push:   reject qty==0 / price<=0 / live duplicate id; match; rest remainder
//  - cancel: reject unknown id; emit Cancelled{remaining}
//  - modify: reject unknown id / qty==0 / price<=0;
//            same price && qty <= current -> reduce in place (Reduced, keeps priority);
//            otherwise exactly Cancelled{old qty} + push(id, same side, price, qty)

#include <lob/events.hpp>
#include <lob/types.hpp>

#include <concepts>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace lob {

struct LevelSnapshot {
  Price price;
  std::uint64_t total_qty;
  std::uint32_t order_count;
  friend auto operator==(const LevelSnapshot&, const LevelSnapshot&) -> bool = default;
};

template <class B>
concept OrderBook = requires(B& b, const B& cb, OrderId id, Side side, Price price, Qty qty) {
  { B::name } -> std::convertible_to<std::string_view>;
  b.push(id, side, price, qty);
  b.cancel(id);
  b.modify(id, price, qty);
  { cb.best_bid() } -> std::same_as<std::optional<Price>>;
  { cb.best_ask() } -> std::same_as<std::optional<Price>>;
  { cb.order_count() } -> std::convertible_to<std::size_t>;
  // levels in priority order (best first); not on the hot path
  { cb.levels(side) } -> std::same_as<std::vector<LevelSnapshot>>;
  // throws std::logic_error describing the first violated invariant
  cb.check_invariants();
};

}  // namespace lob
