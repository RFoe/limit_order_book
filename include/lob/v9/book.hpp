#pragma once
// v9: v8 without the side dispatch on the common path. v8's add() branched on
// `side` to call match<Buy> / match<Sell> (40% mispredicted on AAPL: the side
// of consecutive pushes is random). Most pushes on real data do not cross, so
// add() first asks may_cross(), computed without branching on side:
//   - the bid bitmap is stored mirrored (bit s ^ (kSlots - 1) marks slot s),
//     so the best opposite slot is min() of either bitmap; side only selects
//     the bitmap and the mirror mask (cmov)
//   - the comparison against the limit is sign-normalised
//   - a non-empty opposite fallback map conservatively counts as "may cross"
// Only pushes that may cross take the original match<Side> dispatch, so the
// remaining side branch sits behind a branch that is almost never taken on
// ITCH data. Everything else is identical to v8; v8's description follows.
//
// v8: v4 without the data-dependent branches that cachegrind's branch
// simulation ranked high in our own code (v4 on AAPL):
//   - unlink: "is the order the head / the tail of its level" (24% of all
//     mispredicts) -> select the link to patch (neighbour node or Level
//     head/tail) with __builtin_unpredictable so clang emits cmov, then store
//   - push_back: "was the level empty" -> same select-then-store
//   - rest: "was the level empty" before setting its bitmap bit (9%) -> set
//     unconditionally (HierBitmap::set is idempotent; its own early exit only
//     depends on the whole 64-slot word being empty)
//   - erase: "did the level become empty" before clearing its bit -> clear_if
//     (removing unlink's branches had made this one unpredictable: it was
//     correlated with them in the branch history)
// Everything else is identical to v4; v4's description follows.
//
// v4: v3 with Loc packed from 24 to 16 bytes (price, node, side), so each
// flat_map slot (pair<const OrderId, Loc>) shrinks from 32 to 24 bytes.
// Everything else is identical to v3; v3's description follows.
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
// Order nodes, the free list and the flat_map index are unchanged from v2.

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

namespace lob::v9 {

template <EventSink Sink, unsigned WindowBits = 16>
class BookT {
 public:
  static constexpr std::string_view name = "v9";

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
    if (tick_ < 1) throw std::invalid_argument("v9: tick must be >= 1");
    grid_ = static_cast<Level*>(buf_.data());
    std::uninitialized_default_construct_n(grid_, kSlots);
    auto* words = reinterpret_cast<std::uint64_t*>(grid_ + kSlots);  // zero-filled by the kernel
    bid_bits_ = Bitmap(words);
    ask_bits_ = Bitmap(words + Bitmap::kTotalWords);
  }

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
    std::optional<Price> best;
    if (!bid_bits_.empty()) best = price_of(bid_bits_.min() ^ kMirror);
    if (!bids_.empty() && (!best || bids_.begin()->first > *best)) best = bids_.begin()->first;
    return best;
  }
  [[nodiscard]] auto best_ask() const -> std::optional<Price> {
    std::optional<Price> best;
    if (!ask_bits_.empty()) best = price_of(ask_bits_.min());
    if (!asks_.empty() && (!best || asks_.begin()->first < *best)) best = asks_.begin()->first;
    return best;
  }
  [[nodiscard]] auto order_count() const -> std::size_t { return index_.size(); }

  [[nodiscard]] auto find(OrderId id) const -> std::optional<OrderView> {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return OrderView{it->second.side, it->second.price, nodes_[it->second.node].qty};
  }

  [[nodiscard]] auto levels(Side side) const -> std::vector<LevelSnapshot> {
    std::vector<LevelSnapshot> out;
    const Bitmap& bits = side == Side::Buy ? bid_bits_ : ask_bits_;
    bits.for_each([&](std::uint32_t b) {
      const std::uint32_t s = b ^ mirror(side);
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
      if (sum != level.total_qty) fail(std::format("level {} total_qty {} != sum {}", price, level.total_qty, sum));
    };
    // grid levels: every marked slot is non-empty; unmarked non-empty slots would
    // hold orders that are never visited, which the order count below catches
    for (Side side : {Side::Buy, Side::Sell}) {
      const Bitmap& bits = side == Side::Buy ? bid_bits_ : ask_bits_;
      if (!bits.consistent()) fail(std::format("{} bitmap summary levels inconsistent", to_string(side)));
      bits.for_each([&](std::uint32_t b) {
        const std::uint32_t s = b ^ mirror(side);
        check_level(grid_[s], price_of(s), side);
      });
    }
    bid_bits_.for_each([&](std::uint32_t b) {
      if (ask_bits_.test(b ^ kMirror)) fail(std::format("slot {} marked on both sides", b ^ kMirror));
    });
    // fallback levels must hold exactly the prices the grid cannot
    auto check_map = [&](const auto& levels, Side side) {
      for (const auto& [price, level] : levels) {
        if (slot_of(price) != kNoSlot) fail(std::format("price {} is on the grid but stored in the map", price));
        check_level(level, price, side);
      }
    };
    check_map(bids_, Side::Buy);
    check_map(asks_, Side::Sell);
    const Price want_buy = asks_.empty() ? kNoMapLevel : asks_.begin()->first;
    const Price want_sell = bids_.empty() ? kNoMapLevel : -bids_.begin()->first;
    if (taker_map_best_[0] != want_buy || taker_map_best_[1] != want_sell)
      fail(std::format("cached map best {}/{} != {}/{}", taker_map_best_[0], taker_map_best_[1], want_buy, want_sell));
    if (orders != index_.size()) fail(std::format("index size {} != orders on book {}", index_.size(), orders));
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
    return std::format("v9 grid: {} slots x {} B + 2 x {}-level bitmap = {} B mapped, pages={} huge_backed={} B, "
                       "tick={} base={} fallback_levels={}",
                       kSlots, sizeof(Level), Bitmap::kDepth, buf_.size(), mem::to_string(buf_.mode()),
                       buf_.huge_bytes(), tick_, anchored_ ? std::format("{}", base_) : std::string("unset"),
                       bids_.size() + asks_.size());
  }

 private:
  static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();
  static constexpr std::uint32_t kNoSlot = std::numeric_limits<std::uint32_t>::max();
  static constexpr std::uint32_t kSlots = std::uint32_t{1} << WindowBits;
  using Bitmap = v3::HierBitmap<WindowBits>;
  // bid bitmap bit = slot ^ kMirror (= kSlots - 1 - slot): best bid is min() like best ask
  static constexpr std::uint32_t kMirror = kSlots - 1;
  static constexpr auto mirror(Side side) -> std::uint32_t { return side == Side::Buy ? kMirror : 0; }

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
  struct Loc {  // 8 + 4 + 1 (+3 padding) = 16 bytes; v3 had side first: 1 + 7 + 8 + 4 + 4 = 24
    Price price;
    std::uint32_t node;
    Side side;
  };
  static_assert(sizeof(Loc) == 16);
  using Index = boost::unordered_flat_map<OrderId, Loc>;
  static_assert(sizeof(typename Index::value_type) == 24, "one flat_map slot");

  [[noreturn]] static void fail(const std::string& what) { throw std::logic_error("v9 invariant: " + what); }

  void reject(OrderId id, OpType op, RejectReason reason) { sink_.on(Rejected{id, op, reason}); }

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

  // Best fallback-map price seen by a taker of each side, sign-normalised like
  // may_cross(): [Buy] = lowest ask, [Sell] = -highest bid; kNoMapLevel when the
  // map is empty. Refreshed on every (cold) change of the maps.
  static constexpr Price kNoMapLevel = std::numeric_limits<Price>::max();
  void refresh_map_best() {
    taker_map_best_[0] = asks_.empty() ? kNoMapLevel : asks_.begin()->first;
    taker_map_best_[1] = bids_.empty() ? kNoMapLevel : -bids_.begin()->first;
  }

  auto level_of(const Loc& loc) -> Level& {
    if (const std::uint32_t s = slot_of(loc.price); s != kNoSlot) return grid_[s];
    return loc.side == Side::Buy ? bids_.find(loc.price)->second : asks_.find(loc.price)->second;
  }

  // ---- node pool ----------------------------------------------------------------
  auto alloc_node(OrderId id, Qty qty) -> std::uint32_t {
    std::uint32_t i = free_head_;
    if (i != kNil) {
      free_head_ = nodes_[i].next;
    } else {
      if (nodes_.size() == kNil) throw std::length_error("v9: order pool exhausted");
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
  // The link to patch is either a neighbour node's field or the Level's
  // head/tail. Select its address branch-free (clang turns an unpredictable
  // ternary into cmov), then store unconditionally. kNil indexes are replaced
  // by 0 before forming the node address so no out-of-range pointer is formed
  // (the pool is never empty here: node i exists).
  void push_back(Level& level, std::uint32_t i) {
    const std::uint32_t tail = level.tail;
    const bool has_tail = __builtin_unpredictable(tail != kNil);
    std::uint32_t* link = has_tail ? &nodes_[has_tail ? tail : 0].next : &level.head;
    *link = i;
    nodes_[i].prev = tail;
    nodes_[i].next = kNil;
    level.tail = i;
    ++level.count;
  }

  void unlink(Level& level, std::uint32_t i) {
    const OrderNode& n = nodes_[i];
    const std::uint32_t prev = n.prev;
    const std::uint32_t next = n.next;
    const bool has_prev = __builtin_unpredictable(prev != kNil);
    const bool has_next = __builtin_unpredictable(next != kNil);
    std::uint32_t* to_next = has_prev ? &nodes_[has_prev ? prev : 0].next : &level.head;
    std::uint32_t* to_prev = has_next ? &nodes_[has_next ? next : 0].prev : &level.tail;
    *to_next = next;
    *to_prev = prev;
    --level.count;
  }

  // false => match() would consume nothing (no opposite level at or through the
  // limit). Prices are normalised by the taker's sign (sell side negated) so
  // "crosses" is `best <= limit` for both sides; the only data-dependent branch
  // left is "opposite grid empty", which is rare.
  [[nodiscard]] auto may_cross(Side side, Price limit) const -> bool {
    const auto t = static_cast<std::uint32_t>(side);          // Buy = 0, Sell = 1
    const Price sign = 1 - 2 * static_cast<Price>(t);
    const std::uint32_t opp_mirror = kMirror & (0U - t);       // bids are mirrored
    const Bitmap& opp = __builtin_unpredictable(t != 0) ? bid_bits_ : ask_bits_;
    Price best = taker_map_best_[t];  // already normalised; sentinel when the map is empty
    if (!opp.empty()) [[likely]]
      best = std::min(best, sign * price_of(opp.min() ^ opp_mirror));
    return best <= sign * limit;
  }

  void add(OrderId id, Side side, Price price, Qty qty) {
    if (!may_cross(side, price)) [[likely]] return rest(id, side, price, qty);
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
    constexpr std::uint32_t opp_mirror = buy ? 0 : kMirror;
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
        slot = opp_bits.min() ^ opp_mirror;  // best bid = max slot = min mirrored bit
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
          index_.erase(maker.id);
          unlink(level, head);
          free_node(head);
        }
      }
      if (level.head == kNil) {
        if (use_map) {
          opp_map.erase(opp_map.begin());
          refresh_map_best();
        } else
          opp_bits.clear(slot ^ opp_mirror);
      }
    }
    return qty;
  }

  void rest(OrderId id, Side side, Price price, Qty qty) {
    const std::uint32_t i = alloc_node(id, qty);  // may grow nodes_: take references after
    if (!anchored_ && static_cast<std::uint64_t>(price) % static_cast<std::uint64_t>(tick_) == 0) anchor(price);
    const std::uint32_t s = slot_of(price);
    Level* level = nullptr;
    if (s != kNoSlot) {
      level = &grid_[s];
      (side == Side::Buy ? bid_bits_ : ask_bits_).set(s ^ mirror(side));  // idempotent: no "was it empty" branch
    } else {
      level = side == Side::Buy ? &bids_[price] : &asks_[price];
      refresh_map_best();
    }
    push_back(*level, i);
    level->total_qty += qty;
    index_.emplace(id, Loc{.price = price, .node = i, .side = side});
    sink_.on(Rested{id, side, price, qty});
  }

  void erase(typename Index::iterator it) {
    const Loc& loc = it->second;
    if (const std::uint32_t s = slot_of(loc.price); s != kNoSlot) {
      Level& level = grid_[s];
      level.total_qty -= nodes_[loc.node].qty;
      unlink(level, loc.node);
      free_node(loc.node);
      // "did the level become empty" is unpredictable once unlink is branch-free
      // (it used to be inferable from unlink's prev/next branches)
      (loc.side == Side::Buy ? bid_bits_ : ask_bits_).clear_if(s ^ mirror(loc.side), level.head == kNil);
    } else {
      auto remove_from = [&](auto& levels) {
        auto level_it = levels.find(loc.price);
        Level& level = level_it->second;
        level.total_qty -= nodes_[loc.node].qty;
        unlink(level, loc.node);
        free_node(loc.node);
        if (level.head == kNil) {
          levels.erase(level_it);
          refresh_map_best();
        }
      };
      if (loc.side == Side::Buy)
        remove_from(bids_);
      else
        remove_from(asks_);
    }
    index_.erase(it);
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
  Price taker_map_best_[2] = {kNoMapLevel, kNoMapLevel};
  std::vector<OrderNode> nodes_;
  std::uint32_t free_head_ = kNil;
  Index index_;
};

template <class Sink>
using Book = BookT<Sink>;

static_assert(OrderBook<Book<RecordingSink>>);
static_assert(OrderBook<Book<ChecksumSink>>);

}  // namespace lob::v9
