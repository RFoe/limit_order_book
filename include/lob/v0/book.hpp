#pragma once
// v0: the reference implementation. Optimised for being obviously correct, not
// fast. Every later version is differentially tested against it.
//
//   bids_/asks_ : std::map<Price, Level>       (best level = begin())
//   Level       : std::list<Order> + total_qty (FIFO queue)
//   index_      : std::unordered_map<OrderId, Loc> (O(1) lookup for cancel/modify)

#include <lob/book_concept.hpp>
#include <lob/events.hpp>
#include <lob/types.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lob::v0 {

template <EventSink Sink>
class Book {
 public:
  static constexpr std::string_view name = "v0";

  struct OrderView {
    Side side;
    Price price;
    Qty qty;
  };

  explicit Book(Sink& sink) : sink_(sink) {}

  void push(OrderId id, Side side, Price price, Qty qty) {
    if (qty == 0) return reject(id, OpType::Push, RejectReason::InvalidQty);
    if (price <= 0) return reject(id, OpType::Push, RejectReason::InvalidPrice);
    if (index_.contains(id)) return reject(id, OpType::Push, RejectReason::DuplicateId);
    add(id, side, price, qty);
  }

  void cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return reject(id, OpType::Cancel, RejectReason::UnknownId);
    const Qty remaining = it->second.order->qty;
    erase(it);
    sink_.on(Cancelled{id, remaining});
  }

  void modify(OrderId id, Price price, Qty qty) {
    auto it = index_.find(id);
    if (it == index_.end()) return reject(id, OpType::Modify, RejectReason::UnknownId);
    if (qty == 0) return reject(id, OpType::Modify, RejectReason::InvalidQty);
    if (price <= 0) return reject(id, OpType::Modify, RejectReason::InvalidPrice);

    Loc& loc = it->second;
    if (price == loc.price && qty <= loc.order->qty) {  // reduce: keep priority
      level_of(loc).total_qty -= loc.order->qty - qty;
      loc.order->qty = qty;
      sink_.on(Reduced{id, qty});
      return;
    }
    const Side side = loc.side;
    const Qty old_qty = loc.order->qty;
    erase(it);
    sink_.on(Cancelled{id, old_qty});
    add(id, side, price, qty);
  }

  [[nodiscard]] auto best_bid() const -> std::optional<Price> {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
  }
  [[nodiscard]] auto best_ask() const -> std::optional<Price> {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
  }
  [[nodiscard]] auto order_count() const -> std::size_t { return index_.size(); }

  [[nodiscard]] auto find(OrderId id) const -> std::optional<OrderView> {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return OrderView{it->second.side, it->second.price, it->second.order->qty};
  }

  [[nodiscard]] auto levels(Side side) const -> std::vector<LevelSnapshot> {
    std::vector<LevelSnapshot> out;
    auto collect = [&](const auto& levels) {
      for (const auto& [price, level] : levels)
        out.push_back({price, level.total_qty, static_cast<std::uint32_t>(level.orders.size())});
    };
    if (side == Side::Buy)
      collect(bids_);
    else
      collect(asks_);
    return out;
  }

  void check_invariants() const {
    std::size_t orders = 0;
    auto check_side = [&](const auto& levels, Side side) {
      for (const auto& [price, level] : levels) {
        if (level.orders.empty()) fail(std::format("empty level {} on {}", price, to_string(side)));
        std::uint64_t sum = 0;
        for (auto o = level.orders.begin(); o != level.orders.end(); ++o) {
          if (o->qty == 0) fail(std::format("order {} has qty 0", o->id));
          sum += o->qty;
          auto idx = index_.find(o->id);
          if (idx == index_.end()) fail(std::format("order {} missing from index", o->id));
          if (idx->second.side != side || idx->second.price != price || idx->second.order != o)
            fail(std::format("index entry of order {} is stale", o->id));
          ++orders;
        }
        if (sum != level.total_qty)
          fail(std::format("level {} total_qty {} != sum {}", price, level.total_qty, sum));
      }
    };
    check_side(bids_, Side::Buy);
    check_side(asks_, Side::Sell);
    if (orders != index_.size()) fail(std::format("index size {} != orders on book {}", index_.size(), orders));
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first)
      fail(std::format("crossed book: bid {} >= ask {}", bids_.begin()->first, asks_.begin()->first));
  }

 private:
  struct Order {
    OrderId id;
    Qty qty;
  };
  struct Level {
    std::list<Order> orders;
    std::uint64_t total_qty = 0;
  };
  using Bids = std::map<Price, Level, std::greater<>>;
  using Asks = std::map<Price, Level, std::less<>>;
  struct Loc {
    Side side;
    Price price;
    typename std::list<Order>::iterator order;
  };
  using Index = std::unordered_map<OrderId, Loc>;

  [[noreturn]] static void fail(const std::string& what) { throw std::logic_error("v0 invariant: " + what); }

  void reject(OrderId id, OpType op, RejectReason reason) { sink_.on(Rejected{id, op, reason}); }

  auto level_of(const Loc& loc) -> Level& {
    return loc.side == Side::Buy ? bids_.find(loc.price)->second : asks_.find(loc.price)->second;
  }

  void add(OrderId id, Side side, Price price, Qty qty) {
    if (side == Side::Buy)
      qty = match(asks_, id, qty, [price](Price ask) { return ask <= price; });
    else
      qty = match(bids_, id, qty, [price](Price bid) { return bid >= price; });
    if (qty == 0) return;
    if (side == Side::Buy)
      rest(bids_, id, side, price, qty);
    else
      rest(asks_, id, side, price, qty);
  }

  template <class Levels, class Crosses>
  auto match(Levels& opposite_levels, OrderId taker, Qty qty, Crosses crosses) -> Qty {
    while (qty > 0 && !opposite_levels.empty()) {
      auto level_it = opposite_levels.begin();
      if (!crosses(level_it->first)) break;
      Level& level = level_it->second;
      while (qty > 0 && !level.orders.empty()) {
        Order& maker = level.orders.front();
        const Qty fill = std::min(qty, maker.qty);
        sink_.on(Trade{taker, maker.id, level_it->first, fill});
        qty -= fill;
        maker.qty -= fill;
        level.total_qty -= fill;
        if (maker.qty == 0) {
          index_.erase(maker.id);
          level.orders.pop_front();
        }
      }
      if (level.orders.empty()) opposite_levels.erase(level_it);
    }
    return qty;
  }

  template <class Levels>
  void rest(Levels& levels, OrderId id, Side side, Price price, Qty qty) {
    Level& level = levels[price];
    level.orders.push_back(Order{id, qty});
    level.total_qty += qty;
    index_.emplace(id, Loc{side, price, std::prev(level.orders.end())});
    sink_.on(Rested{id, side, price, qty});
  }

  void erase(typename Index::iterator it) {
    const Loc& loc = it->second;
    auto remove_from = [&](auto& levels) {
      auto level_it = levels.find(loc.price);
      Level& level = level_it->second;
      level.total_qty -= loc.order->qty;
      level.orders.erase(loc.order);
      if (level.orders.empty()) levels.erase(level_it);
    };
    if (loc.side == Side::Buy)
      remove_from(bids_);
    else
      remove_from(asks_);
    index_.erase(it);
  }

  Sink& sink_;
  Bids bids_;
  Asks asks_;
  Index index_;
};

static_assert(OrderBook<Book<RecordingSink>>);
static_assert(OrderBook<Book<ChecksumSink>>);

}  // namespace lob::v0
