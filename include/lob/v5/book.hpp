#pragma once
// v5: v4 with an 8-byte index slot.
//
//   index_       : boost::unordered_flat_map<uint32_t, uint32_t>
//                  key   = id - id_base_ (id_base_ anchored on the first resting
//                          order, 2^31 below it, so ids slightly below the first
//                          one still encode)
//                  value = order node index
//   wide_index_  : boost::unordered_flat_map<OrderId, uint32_t> for ids outside
//                  [id_base_, id_base_ + 2^32); which map holds an id depends on
//                  the id alone, so duplicate detection stays exact
//   OrderNode    : now carries price and side (24 -> 32 bytes), which v4 kept in
//                  the 16-byte Loc; Loc is gone
// Lookups still probe once (find + erase through the iterator). Price grid,
// bitmaps, fallback level map and free list are unchanged from v4; v4's own
// description follows.
//
// v3: v2 with the price levels moved from std::map into a tick-indexed array.
//
//   grid_       : Level[2^WindowBits], one slot per tick, shared by both sides
//                 (a price can rest on one side only: the book never crosses)
//   bid/ask bits: one n-level 64-ary HierBitmap per side marking non-empty
//                 slots; best bid = max(), best ask = min() via clz/ctz
//   window      : anchored once, centred on the first on-grid resting price
//                 (no look-ahead); slot = (price - base) / tick
//   fallback    : prices off the tick grid or outside the window use the v2
//                 std::map levels unchanged; best price = better of the two
//   memory      : grid + bitmaps are one HugeBuffer (2 MiB pages if possible,
//                 pre-faulted at construction, outside any measurement)

#include <lob/book_concept.hpp>
#include <lob/book_config.hpp>
#include <lob/events.hpp>
#include <lob/mem/huge_buffer.hpp>
#include <lob/types.hpp>
#include <lob/v3/hier_bitmap.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

namespace lob::v5 {

template <EventSink Sink, unsigned WindowBits = 16>
class BookT {
 public:
  static constexpr std::string_view name = "v5";

  struct OrderView {
    Side side;
    Price price;
    Qty qty;
  };

  explicit BookT(Sink& sink) : BookT(sink, BookConfig{}) {}

  BookT(Sink& sink, const BookConfig& cfg)
      : sink_(sink),
        buf_(kSlots * sizeof(Level) + 2 * Bitmap::kTotalWords * sizeof(std::uint64_t)),
        tick_(cfg.tick) {
    if (tick_ < 1) throw std::invalid_argument("v5: tick must be >= 1");
    grid_ = static_cast<Level*>(buf_.data());
    std::uninitialized_default_construct_n(grid_, kSlots);
    auto* words = reinterpret_cast<std::uint64_t*>(grid_ + kSlots);  // zero-filled by the kernel
    bid_bits_ = Bitmap(words);
    ask_bits_ = Bitmap(words + Bitmap::kTotalWords);
  }

  void push(OrderId id, Side side, Price price, Qty qty) {
    if (qty == 0) return reject(id, OpType::Push, RejectReason::InvalidQty);
    if (price <= 0) return reject(id, OpType::Push, RejectReason::InvalidPrice);
    if (index_contains(id)) return reject(id, OpType::Push, RejectReason::DuplicateId);
    add(id, side, price, qty);
  }

  void cancel(OrderId id) {
    std::uint32_t k = 0;
    if (compact_key(id, k))
      cancel_in(index_, k, id);
    else
      cancel_in(wide_index_, id, id);
  }

  void modify(OrderId id, Price price, Qty qty) {
    std::uint32_t k = 0;
    if (compact_key(id, k))
      modify_in(index_, k, id, price, qty);
    else
      modify_in(wide_index_, id, id, price, qty);
  }

  [[nodiscard]] auto best_bid() const -> std::optional<Price> {
    std::optional<Price> best;
    if (!bid_bits_.empty()) best = price_of(bid_bits_.max());
    if (!bids_.empty() && (!best || bids_.begin()->first > *best)) best = bids_.begin()->first;
    return best;
  }
  [[nodiscard]] auto best_ask() const -> std::optional<Price> {
    std::optional<Price> best;
    if (!ask_bits_.empty()) best = price_of(ask_bits_.min());
    if (!asks_.empty() && (!best || asks_.begin()->first < *best)) best = asks_.begin()->first;
    return best;
  }
  [[nodiscard]] auto order_count() const -> std::size_t { return index_.size() + wide_index_.size(); }

  [[nodiscard]] auto find(OrderId id) const -> std::optional<OrderView> {
    const std::uint32_t i = index_lookup(id);
    if (i == kNil) return std::nullopt;
    return OrderView{nodes_[i].side, nodes_[i].price, nodes_[i].qty};
  }

  [[nodiscard]] auto levels(Side side) const -> std::vector<LevelSnapshot> {
    std::vector<LevelSnapshot> out;
    const Bitmap& bits = side == Side::Buy ? bid_bits_ : ask_bits_;
    bits.for_each([&](std::uint32_t s) {
      out.push_back({price_of(s), grid_[s].total_qty, grid_[s].count});
    });
    auto collect = [&](const auto& levels) {
      for (const auto& [price, level] : levels) out.push_back({price, level.total_qty, level.count});
    };
    if (side == Side::Buy)
      collect(bids_);
    else
      collect(asks_);
    if (side == Side::Buy)
      std::ranges::sort(out, std::greater<>{}, &LevelSnapshot::price);
    else
      std::ranges::sort(out, std::less<>{}, &LevelSnapshot::price);
    return out;
  }

  void check_invariants() const {
    std::size_t orders = 0;
    auto check_level = [&](const Level& level, Price price, Side side) {
      if (level.count == 0 || level.head == kNil) fail(std::format("empty level {} on {}", price, to_string(side)));
      std::uint64_t sum = 0;
      std::uint32_t n = 0;
      std::uint32_t prev = kNil;
      for (std::uint32_t i = level.head; i != kNil; prev = i, i = nodes_[i].next) {
        const OrderNode& o = nodes_[i];
        if (o.prev != prev) fail(std::format("node {} (order {}) has a broken prev link", i, o.id));
        if (o.qty == 0) fail(std::format("order {} has qty 0", o.id));
        if (o.side != side || o.price != price) fail(std::format("node of order {} disagrees with its level", o.id));
        sum += o.qty;
        ++n;
        if (index_lookup(o.id) != i) fail(std::format("index entry of order {} missing or stale", o.id));
        ++orders;
      }
      if (prev != level.tail) fail(std::format("level {} tail {} != last node {}", price, level.tail, prev));
      if (n != level.count) fail(std::format("level {} count {} != nodes {}", price, level.count, n));
      if (sum != level.total_qty) fail(std::format("level {} total_qty {} != sum {}", price, level.total_qty, sum));
    };
    for (Side side : {Side::Buy, Side::Sell}) {
      const Bitmap& bits = side == Side::Buy ? bid_bits_ : ask_bits_;
      if (!bits.consistent()) fail(std::format("{} bitmap summary levels inconsistent", to_string(side)));
      bits.for_each([&](std::uint32_t s) { check_level(grid_[s], price_of(s), side); });
    }
    for (std::size_t w = 0; w < Bitmap::kLeafWords; ++w)
      if ((bid_bits_.leaf_word(w) & ask_bits_.leaf_word(w)) != 0)
        fail(std::format("slot word {} marked on both sides", w));
    auto check_map = [&](const auto& levels, Side side) {
      for (const auto& [price, level] : levels) {
        if (slot_of(price) != kNoSlot) fail(std::format("price {} is on the grid but stored in the map", price));
        check_level(level, price, side);
      }
    };
    check_map(bids_, Side::Buy);
    check_map(asks_, Side::Sell);
    // every index entry points at a node carrying that id, and lives in the map
    // its id selects
    for (const auto& [k, i] : index_)
      if (i >= nodes_.size() || nodes_[i].id != id_base_ + k)
        fail(std::format("compact index entry {} -> node {} is stale", k, i));
    for (const auto& [id, i] : wide_index_) {
      std::uint32_t k = 0;
      if (compact_key(id, k)) fail(std::format("order {} is compact-encodable but in the wide index", id));
      if (i >= nodes_.size() || nodes_[i].id != id) fail(std::format("wide index entry {} is stale", id));
    }
    if (orders != order_count()) fail(std::format("index size {} != orders on book {}", order_count(), orders));
    std::size_t free_nodes = 0;
    for (std::uint32_t i = free_head_; i != kNil; i = nodes_[i].next) ++free_nodes;
    if (free_nodes + orders != nodes_.size())
      fail(std::format("pool leak: {} free + {} live != {} nodes", free_nodes, orders, nodes_.size()));
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (bid && ask && *bid >= *ask) fail(std::format("crossed book: bid {} >= ask {}", *bid, *ask));
  }

  // not part of the OrderBook concept; printed by `book replay`
  [[nodiscard]] auto memory_info() const -> std::string {
    return std::format("v5 grid: {} slots x {} B, pages={} huge_backed={} B, tick={} base={} fallback_levels={}; "
                       "index: {} compact (8 B slots, buckets {}) + {} wide, id_base={}, node {} B",
                       kSlots, sizeof(Level), mem::to_string(buf_.mode()), buf_.huge_bytes(), tick_,
                       anchored_ ? std::format("{}", base_) : std::string("unset"), bids_.size() + asks_.size(),
                       index_.size(), index_.bucket_count(), wide_index_.size(),
                       id_anchored_ ? std::format("{}", id_base_) : std::string("unset"), sizeof(OrderNode));
  }

 private:
  static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();
  static constexpr std::uint32_t kNoSlot = std::numeric_limits<std::uint32_t>::max();
  static constexpr std::uint32_t kSlots = std::uint32_t{1} << WindowBits;
  using Bitmap = v3::HierBitmap<WindowBits>;

  struct OrderNode {
    OrderId id;
    Price price;
    Qty qty;
    std::uint32_t next;  // level list: towards the tail; free list: next free node
    std::uint32_t prev;  // level list only
    Side side;
  };
  static_assert(sizeof(OrderNode) == 32);
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
  using Index = boost::unordered_flat_map<std::uint32_t, std::uint32_t>;
  using WideIndex = boost::unordered_flat_map<OrderId, std::uint32_t>;
  static_assert(sizeof(typename Index::value_type) == 8, "one compact index slot");

  [[noreturn]] static void fail(const std::string& what) { throw std::logic_error("v5 invariant: " + what); }

  void reject(OrderId id, OpType op, RejectReason reason) { sink_.on(Rejected{id, op, reason}); }

  // ---- order index --------------------------------------------------------------
  // compact key for id, if id lies in [id_base_, id_base_ + 2^32)
  [[nodiscard]] auto compact_key(OrderId id, std::uint32_t& k) const -> bool {
    if (!id_anchored_ || id < id_base_ || id - id_base_ > 0xFFFF'FFFFULL) return false;
    k = static_cast<std::uint32_t>(id - id_base_);
    return true;
  }

  [[nodiscard]] auto index_contains(OrderId id) const -> bool {
    std::uint32_t k = 0;
    return compact_key(id, k) ? index_.contains(k) : wide_index_.contains(id);
  }

  [[nodiscard]] auto index_lookup(OrderId id) const -> std::uint32_t {
    std::uint32_t k = 0;
    if (compact_key(id, k)) {
      auto it = index_.find(k);
      return it == index_.end() ? kNil : it->second;
    }
    auto it = wide_index_.find(id);
    return it == wide_index_.end() ? kNil : it->second;
  }

  void index_insert(OrderId id, std::uint32_t node) {
    if (!id_anchored_) {  // 2^31 of headroom below the first id
      id_base_ = id >= (OrderId{1} << 31) ? id - (OrderId{1} << 31) : 0;
      id_anchored_ = true;
    }
    std::uint32_t k = 0;
    if (compact_key(id, k))
      index_.emplace(k, node);
    else
      wide_index_.emplace(id, node);
  }

  void index_erase(OrderId id) {
    std::uint32_t k = 0;
    if (compact_key(id, k))
      index_.erase(k);
    else
      wide_index_.erase(id);
  }

  template <class Map, class Key>
  void cancel_in(Map& map, Key k, OrderId id) {
    auto it = map.find(k);
    if (it == map.end()) return reject(id, OpType::Cancel, RejectReason::UnknownId);
    const std::uint32_t node = it->second;
    const Qty remaining = nodes_[node].qty;
    remove_node(node);
    map.erase(it);
    sink_.on(Cancelled{id, remaining});
  }

  template <class Map, class Key>
  void modify_in(Map& map, Key k, OrderId id, Price price, Qty qty) {
    auto it = map.find(k);
    if (it == map.end()) return reject(id, OpType::Modify, RejectReason::UnknownId);
    if (qty == 0) return reject(id, OpType::Modify, RejectReason::InvalidQty);
    if (price <= 0) return reject(id, OpType::Modify, RejectReason::InvalidPrice);

    const std::uint32_t node = it->second;
    OrderNode& n = nodes_[node];
    if (price == n.price && qty <= n.qty) {  // reduce: keep priority
      level_of(n).total_qty -= n.qty - qty;
      n.qty = qty;
      sink_.on(Reduced{id, qty});
      return;
    }
    const Side side = n.side;
    const Qty old_qty = n.qty;
    remove_node(node);
    map.erase(it);
    sink_.on(Cancelled{id, old_qty});
    add(id, side, price, qty);
  }

  // ---- price grid ---------------------------------------------------------------
  [[nodiscard]] auto slot_of(Price price) const -> std::uint32_t {
    if (!anchored_) return kNoSlot;
    const Price d = price - base_;
    if (d < 0) return kNoSlot;
    const auto ud = static_cast<std::uint64_t>(d);
    const auto ut = static_cast<std::uint64_t>(tick_);
    if (ud % ut != 0) return kNoSlot;
    const std::uint64_t s = ud / ut;
    return s < kSlots ? static_cast<std::uint32_t>(s) : kNoSlot;
  }
  [[nodiscard]] auto price_of(std::uint32_t slot) const -> Price { return base_ + static_cast<Price>(slot) * tick_; }

  void anchor(Price price) {  // centre the window on the first on-grid resting price
    base_ = price - static_cast<Price>(kSlots / 2) * tick_;
    anchored_ = true;
  }

  auto level_of(const OrderNode& n) -> Level& {
    if (const std::uint32_t s = slot_of(n.price); s != kNoSlot) return grid_[s];
    return n.side == Side::Buy ? bids_.find(n.price)->second : asks_.find(n.price)->second;
  }

  // ---- node pool ----------------------------------------------------------------
  auto alloc_node(OrderId id, Side side, Price price, Qty qty) -> std::uint32_t {
    std::uint32_t i = free_head_;
    if (i != kNil) {
      free_head_ = nodes_[i].next;
    } else {
      if (nodes_.size() == kNil) throw std::length_error("v5: order pool exhausted");
      i = static_cast<std::uint32_t>(nodes_.size());
      nodes_.emplace_back();
    }
    nodes_[i] = OrderNode{id, price, qty, kNil, kNil, side};
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
      qty = match<Side::Buy>(id, price, qty);
    else
      qty = match<Side::Sell>(id, price, qty);
    if (qty == 0) return;
    rest(id, side, price, qty);
  }

  // Consume opposite levels best-first; the best level is the better of the
  // grid's (bitmap min/max) and the fallback map's first entry.
  template <Side TakerSide>
  auto match(OrderId taker, Price limit, Qty qty) -> Qty {
    constexpr bool buy = TakerSide == Side::Buy;
    Bitmap& opp_bits = buy ? ask_bits_ : bid_bits_;
    auto& opp_map = [this]() -> auto& {
      if constexpr (buy)
        return asks_;
      else
        return bids_;
    }();
    while (qty > 0) {
      const bool have_grid = !opp_bits.empty();
      const bool have_map = !opp_map.empty();
      if (!have_grid && !have_map) break;
      std::uint32_t slot = kNoSlot;
      Price price{};
      if (have_grid) {
        slot = buy ? opp_bits.min() : opp_bits.max();
        price = price_of(slot);
      }
      const bool use_map =
          have_map && (!have_grid || (buy ? opp_map.begin()->first < price : opp_map.begin()->first > price));
      if (use_map) price = opp_map.begin()->first;
      if (buy ? price > limit : price < limit) break;
      Level& level = use_map ? opp_map.begin()->second : grid_[slot];
      while (qty > 0 && level.head != kNil) {
        const std::uint32_t head = level.head;
        OrderNode& maker = nodes_[head];
        const Qty fill = std::min(qty, maker.qty);
        sink_.on(Trade{taker, maker.id, price, fill});
        qty -= fill;
        maker.qty -= fill;
        level.total_qty -= fill;
        if (maker.qty == 0) {
          index_erase(maker.id);
          unlink(level, head);
          free_node(head);
        }
      }
      if (level.head == kNil) {
        if (use_map)
          opp_map.erase(opp_map.begin());
        else
          opp_bits.clear(slot);
      }
    }
    return qty;
  }

  void rest(OrderId id, Side side, Price price, Qty qty) {
    const std::uint32_t i = alloc_node(id, side, price, qty);  // may grow nodes_: take references after
    if (!anchored_ && static_cast<std::uint64_t>(price) % static_cast<std::uint64_t>(tick_) == 0) anchor(price);
    const std::uint32_t s = slot_of(price);
    Level* level = nullptr;
    if (s != kNoSlot) {
      level = &grid_[s];
      if (level->head == kNil) (side == Side::Buy ? bid_bits_ : ask_bits_).set(s);
    } else {
      level = side == Side::Buy ? &bids_[price] : &asks_[price];
    }
    push_back(*level, i);
    level->total_qty += qty;
    index_insert(id, i);
    sink_.on(Rested{id, side, price, qty});
  }

  // unlink node i from its level (grid or fallback map) and free it; the index
  // entry is removed by the caller
  void remove_node(std::uint32_t i) {
    const OrderNode& n = nodes_[i];
    if (const std::uint32_t s = slot_of(n.price); s != kNoSlot) {
      Level& level = grid_[s];
      level.total_qty -= n.qty;
      const Side side = n.side;
      unlink(level, i);
      free_node(i);
      if (level.head == kNil) (side == Side::Buy ? bid_bits_ : ask_bits_).clear(s);
    } else {
      auto remove_from = [&](auto& levels) {
        auto level_it = levels.find(n.price);
        Level& level = level_it->second;
        level.total_qty -= n.qty;
        unlink(level, i);
        free_node(i);
        if (level.head == kNil) levels.erase(level_it);
      };
      if (n.side == Side::Buy)
        remove_from(bids_);
      else
        remove_from(asks_);
    }
  }

  Sink& sink_;
  mem::HugeBuffer buf_;  // grid_ + both bitmaps
  Level* grid_ = nullptr;
  Bitmap bid_bits_;
  Bitmap ask_bits_;
  Price tick_;
  Price base_ = 0;
  bool anchored_ = false;
  Bids bids_;  // fallback: off-grid / out-of-window prices
  Asks asks_;
  std::vector<OrderNode> nodes_;
  std::uint32_t free_head_ = kNil;
  Index index_;
  WideIndex wide_index_;
  OrderId id_base_ = 0;
  bool id_anchored_ = false;
};

template <class Sink>
using Book = BookT<Sink>;

static_assert(OrderBook<Book<RecordingSink>>);
static_assert(OrderBook<Book<ChecksumSink>>);

}  // namespace lob::v5
