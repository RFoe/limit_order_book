#pragma once
// v2: v1 with std::list<Order> replaced by an intrusive doubly linked list over
// a node pool. Nodes live in one flat std::vector and link to each other by
// uint32_t index (stable across vector growth, half the size of a pointer).
// Two kinds of lists thread through the pool:
//   - level list: FIFO queue of one price level (head/tail in Level, next/prev)
//   - free list : released nodes (singly linked through next), reused LIFO so a
//                 new order lands in a recently touched, likely cached node
// The price-level map (std::map) and the index (boost::unordered_flat_map) are
// unchanged from v1 on purpose.
//
//   bids_/asks_ : std::map<Price, Level>                 (best level = begin())
//   Level       : head/tail node index + total_qty + order count
//   nodes_      : std::vector<OrderNode>  (pool; free list via free_head_)
//   index_      : boost::unordered_flat_map<OrderId, Loc>

#include <lob/book_concept.hpp>
#include <lob/events.hpp>
#include <lob/types.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

namespace lob::v2 {

template <EventSink Sink>
class Book {
 public:
  static constexpr std::string_view name = "v2";

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
    const Qty remaining = nodes_[it->second.node].qty;
    erase(it);
    sink_.on(Cancelled{id, remaining});
  }

  void modify(OrderId id, Price price, Qty qty) {
    auto it = index_.find(id);
    if (it == index_.end()) return reject(id, OpType::Modify, RejectReason::UnknownId);
    if (qty == 0) return reject(id, OpType::Modify, RejectReason::InvalidQty);
    if (price <= 0) return reject(id, OpType::Modify, RejectReason::InvalidPrice);

    Loc& loc = it->second;
    OrderNode& node = nodes_[loc.node];
    if (price == loc.price && qty <= node.qty) {  // reduce: keep priority
      level_of(loc).total_qty -= node.qty - qty;
      node.qty = qty;
      sink_.on(Reduced{id, qty});
      return;
    }
    const Side side = loc.side;
    const Qty old_qty = node.qty;
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
    return OrderView{it->second.side, it->second.price, nodes_[it->second.node].qty};
  }

  [[nodiscard]] auto levels(Side side) const -> std::vector<LevelSnapshot> {
    std::vector<LevelSnapshot> out;
    auto collect = [&](const auto& levels) {
      for (const auto& [price, level] : levels) out.push_back({price, level.total_qty, level.count});
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
        if (level.count == 0 || level.head == kNil)
          fail(std::format("empty level {} on {}", price, to_string(side)));
        std::uint64_t sum = 0;
        std::uint32_t n = 0;
        std::uint32_t prev = kNil;
        for (std::uint32_t i = level.head; i != kNil; prev = i, i = nodes_[i].next) {
          const OrderNode& o = nodes_[i];
          if (o.prev != prev) fail(std::format("node {} (order {}) has a broken prev link", i, o.id));
          if (o.qty == 0) fail(std::format("order {} has qty 0", o.id));
          sum += o.qty;
          ++n;
          auto idx = index_.find(o.id);
          if (idx == index_.end()) fail(std::format("order {} missing from index", o.id));
          if (idx->second.side != side || idx->second.price != price || idx->second.node != i)
            fail(std::format("index entry of order {} is stale", o.id));
          ++orders;
        }
        if (prev != level.tail) fail(std::format("level {} tail {} != last node {}", price, level.tail, prev));
        if (n != level.count) fail(std::format("level {} count {} != nodes {}", price, level.count, n));
        if (sum != level.total_qty)
          fail(std::format("level {} total_qty {} != sum {}", price, level.total_qty, sum));
      }
    };
    check_side(bids_, Side::Buy);
    check_side(asks_, Side::Sell);
    if (orders != index_.size()) fail(std::format("index size {} != orders on book {}", index_.size(), orders));
    std::size_t free_nodes = 0;
    for (std::uint32_t i = free_head_; i != kNil; i = nodes_[i].next) ++free_nodes;
    if (free_nodes + orders != nodes_.size())
      fail(std::format("pool leak: {} free + {} live != {} nodes", free_nodes, orders, nodes_.size()));
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first)
      fail(std::format("crossed book: bid {} >= ask {}", bids_.begin()->first, asks_.begin()->first));
  }

 private:
  static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

  struct OrderNode {
    OrderId id;
    Qty qty;
    std::uint32_t next;  // level list: towards the tail; free list: next free node
    std::uint32_t prev;  // level list only
  };
  static_assert(sizeof(OrderNode) == 24);
  static_assert(sizeof(OrderNode) <= std::hardware_destructive_interference_size / 2,
                "keep several order nodes per cache line");

  struct Level {
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
    std::uint64_t total_qty = 0;
    std::uint32_t count = 0;
  };
  using Bids = std::map<Price, Level, std::greater<>>;
  using Asks = std::map<Price, Level, std::less<>>;
  struct Loc {
    Side side;
    Price price;
    std::uint32_t node;
  };
  using Index = boost::unordered_flat_map<OrderId, Loc>;

  [[noreturn]] static void fail(const std::string& what) { throw std::logic_error("v2 invariant: " + what); }

  void reject(OrderId id, OpType op, RejectReason reason) { sink_.on(Rejected{id, op, reason}); }

  auto level_of(const Loc& loc) -> Level& {
    return loc.side == Side::Buy ? bids_.find(loc.price)->second : asks_.find(loc.price)->second;
  }

  // ---- node pool ----------------------------------------------------------------
  auto alloc_node(OrderId id, Qty qty) -> std::uint32_t {
    std::uint32_t i = free_head_;
    if (i != kNil) {
      free_head_ = nodes_[i].next;
    } else {
      if (nodes_.size() == kNil) throw std::length_error("v2: order pool exhausted");
      i = static_cast<std::uint32_t>(nodes_.size());
      nodes_.emplace_back();
    }
    nodes_[i] = OrderNode{id, qty, kNil, kNil};
    return i;
  }

  void free_node(std::uint32_t i) {
    nodes_[i].next = free_head_;
    free_head_ = i;
  }

  // ---- level list ---------------------------------------------------------------
  void push_back(Level& level, std::uint32_t i) {
    nodes_[i].prev = level.tail;
    nodes_[i].next = kNil;
    if (level.tail != kNil)
      nodes_[level.tail].next = i;
    else
      level.head = i;
    level.tail = i;
    ++level.count;
  }

  void unlink(Level& level, std::uint32_t i) {
    const OrderNode& n = nodes_[i];
    if (n.prev != kNil)
      nodes_[n.prev].next = n.next;
    else
      level.head = n.next;
    if (n.next != kNil)
      nodes_[n.next].prev = n.prev;
    else
      level.tail = n.prev;
    --level.count;
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
      while (qty > 0 && level.head != kNil) {
        const std::uint32_t head = level.head;
        OrderNode& maker = nodes_[head];
        const Qty fill = std::min(qty, maker.qty);
        sink_.on(Trade{taker, maker.id, level_it->first, fill});
        qty -= fill;
        maker.qty -= fill;
        level.total_qty -= fill;
        if (maker.qty == 0) {
          index_.erase(maker.id);
          unlink(level, head);
          free_node(head);
        }
      }
      if (level.head == kNil) opposite_levels.erase(level_it);
    }
    return qty;
  }

  template <class Levels>
  void rest(Levels& levels, OrderId id, Side side, Price price, Qty qty) {
    const std::uint32_t i = alloc_node(id, qty);  // may grow nodes_: take references after
    Level& level = levels[price];
    push_back(level, i);
    level.total_qty += qty;
    index_.emplace(id, Loc{side, price, i});
    sink_.on(Rested{id, side, price, qty});
  }

  void erase(typename Index::iterator it) {
    const Loc& loc = it->second;
    auto remove_from = [&](auto& levels) {
      auto level_it = levels.find(loc.price);
      Level& level = level_it->second;
      level.total_qty -= nodes_[loc.node].qty;
      unlink(level, loc.node);
      free_node(loc.node);
      if (level.head == kNil) levels.erase(level_it);
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
  std::vector<OrderNode> nodes_;
  std::uint32_t free_head_ = kNil;
  Index index_;
};

static_assert(OrderBook<Book<RecordingSink>>);
static_assert(OrderBook<Book<ChecksumSink>>);

}  // namespace lob::v2
