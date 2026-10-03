#include <lob/workload/generator.hpp>

#include <lob/events.hpp>
#include <lob/replay.hpp>
#include <lob/v0/book.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace lob::workload {

// ---- Rng --------------------------------------------------------------------

Rng::Rng(std::uint64_t seed) noexcept {
  for (auto& s : s_) {  // splitmix64
    seed += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = seed;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    s = z ^ (z >> 31);
  }
}

auto Rng::next() noexcept -> std::uint64_t {
  const std::uint64_t result = std::rotl(s_[1] * 5, 7) * 9;
  const std::uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = std::rotl(s_[3], 45);
  return result;
}

auto Rng::uniform(std::uint64_t n) noexcept -> std::uint64_t {
  return static_cast<std::uint64_t>((static_cast<unsigned __int128>(next()) * n) >> 64);
}

auto Rng::bernoulli(double p) noexcept -> bool {
  if (p <= 0.0) return false;
  if (p >= 1.0) return true;
  // ldexp is exact, so the threshold is identical on every machine
  return next() < static_cast<std::uint64_t>(std::ldexp(p, 64));
}

auto Rng::geometric(double p) noexcept -> std::uint32_t {
  std::uint32_t k = 0;
  while (!bernoulli(p) && k < 1'000) ++k;  // integer-only; p is never tiny here
  return k;
}

auto Rng::exponential(double mean) noexcept -> double {
  // u in (0, 1]; uses std::log, so the sha256 printed by gen_workloads.sh is the
  // cross-machine check
  const double u = static_cast<double>((next() >> 11) + 1) * 0x1.0p-53;
  return -mean * std::log(u);
}

// ---- generator ----------------------------------------------------------------

namespace {

struct LiveOrder {
  Side side;
  Price price;
  Qty qty;
  std::size_t slot;  // position in GenState::ids
};

struct GenState {
  std::unordered_map<OrderId, LiveOrder> live;
  std::vector<OrderId> ids;  // dense list for uniform sampling
  std::uint64_t trades = 0;
  std::uint64_t rejects = 0;

  void add(OrderId id, Side side, Price price, Qty qty) {
    live.emplace(id, LiveOrder{side, price, qty, ids.size()});
    ids.push_back(id);
  }
  void remove(OrderId id) {
    auto it = live.find(id);
    if (it == live.end()) return;
    const std::size_t slot = it->second.slot;
    const OrderId last = ids.back();
    ids[slot] = last;
    live.at(last).slot = slot;
    ids.pop_back();
    live.erase(it);
  }
};

// Keeps GenState in sync with the shadow book.
struct Tracker {
  GenState* st;
  void on(const Trade& t) {
    ++st->trades;
    auto& o = st->live.at(t.maker);
    o.qty -= t.qty;
    if (o.qty == 0) st->remove(t.maker);
  }
  void on(const Rested& r) { st->add(r.id, r.side, r.price, r.qty); }
  void on(const Cancelled& c) { st->remove(c.id); }
  void on(const Reduced& r) { st->live.at(r.id).qty = r.new_qty; }
  void on(const Rejected&) { ++st->rejects; }
};

class Generator {
 public:
  explicit Generator(const GenParams& p) : p_(p), rng_(p.seed), tracker_{&st_}, book_(tracker_), last_mid_(p.mid) {}

  auto run(GenStats* stats) -> std::vector<Op> {
    std::vector<Op> ops;
    ops.reserve(p_.n_ops);
    for (std::uint64_t i = 0; i < p_.n_ops; ++i) {
      const Op op = next_op(i);
      apply(book_, op);
      ops.push_back(op);
    }
    if (st_.rejects != 0) throw std::logic_error(std::format("generator produced {} rejected ops", st_.rejects));
    stats_.trades = st_.trades;
    stats_.final_orders = book_.order_count();
    if (stats) *stats = stats_;
    return ops;
  }

 private:
  auto next_op(std::uint64_t i) -> Op {
    if (i < p_.warmup) return push_op(i, /*allow_marketable=*/false);
    if (auto op = expired_cancel(i)) return *op;
    if (rng_.bernoulli(p_.p_modify)) {
      if (auto op = modify_op()) return *op;
      ++stats_.fallbacks;
    }
    return push_op(i, /*allow_marketable=*/true);
  }

  auto push_op(std::uint64_t i, bool allow_marketable) -> Op {
    const Side side = rng_.bernoulli(p_.p_buy) ? Side::Buy : Side::Sell;
    const bool buy = side == Side::Buy;
    const auto bid = book_.best_bid();
    const auto ask = book_.best_ask();
    if (bid && ask) last_mid_ = (*bid + *ask) / 2;
    const auto same = buy ? bid : ask;
    const auto opp = buy ? ask : bid;
    const auto dir = buy ? Price{-1} : Price{1};  // "away from the spread" for this side

    Price price{};
    if (allow_marketable && opp && rng_.bernoulli(p_.p_marketable)) {
      ++stats_.marketable;
      price = *opp - dir * static_cast<Price>(rng_.uniform(p_.max_cross_ticks + 1));
    } else if (same) {
      if (opp && (*ask - *bid) > 1 && rng_.bernoulli(p_.p_inside))
        price = *same - dir;  // improve by one tick, still passive
      else
        price = *same + dir * rng_.geometric(p_.p_depth);
    } else {
      const Price ref = opp ? *opp : last_mid_;
      price = ref + dir * (1 + rng_.geometric(p_.p_depth));
    }
    price = std::max<Price>(price, 1);

    const OrderId id = next_id_++;
    // warm-up orders start ageing when the warm-up ends, so they do not all
    // expire in one burst
    const std::uint64_t born = std::max(i, p_.warmup);
    expiry_.emplace(born + static_cast<std::uint64_t>(rng_.exponential(p_.mean_lifetime)), id);
    ++stats_.pushes;
    return make_push(id, side, price, draw_qty());
  }

  // Cancel the earliest-expiring live order if its lifetime is over.
  auto expired_cancel(std::uint64_t i) -> std::optional<Op> {
    while (!expiry_.empty()) {
      const auto [expiry, id] = expiry_.top();
      if (!st_.live.contains(id)) {  // already filled or cancelled
        expiry_.pop();
        continue;
      }
      if (expiry > i) return std::nullopt;
      expiry_.pop();
      ++stats_.cancels;
      return make_cancel(id);
    }
    return std::nullopt;
  }

  auto modify_op() -> std::optional<Op> {
    if (st_.ids.empty()) return std::nullopt;
    const OrderId id = st_.ids[rng_.uniform(st_.ids.size())];
    const LiveOrder& o = st_.live.at(id);
    ++stats_.modifies;
    if (o.qty > 1 && rng_.bernoulli(p_.p_modify_reduce)) {
      ++stats_.reduces;
      return make_modify(id, o.price, static_cast<Qty>(1 + rng_.uniform(o.qty - 1)));
    }
    ++stats_.reprices;
    const auto delta = static_cast<Price>(1 + rng_.uniform(p_.max_reprice_ticks));
    const Price price = std::max<Price>(1, rng_.bernoulli(0.5) ? o.price + delta : o.price - delta);
    return make_modify(id, price, draw_qty());
  }

  auto draw_qty() -> Qty { return p_.lot * (1 + rng_.geometric(p_.p_qty)); }

  using Expiry = std::pair<std::uint64_t, OrderId>;

  GenParams p_;
  Rng rng_;
  GenState st_;
  Tracker tracker_;
  v0::Book<Tracker> book_;
  std::priority_queue<Expiry, std::vector<Expiry>, std::greater<>> expiry_;
  OrderId next_id_ = 1;
  Price last_mid_;
  GenStats stats_;
};

}  // namespace

auto describe(const GenParams& p) -> std::string {
  return std::format(
      "gen seed={} n={} warmup={} p_modify={} p_buy={} p_mkt={} cross={} p_depth={} p_inside={} mid={} "
      "lot={} p_qty={} life={} p_reduce={} reprice={}",
      p.seed, p.n_ops, p.warmup, p.p_modify, p.p_buy, p.p_marketable, p.max_cross_ticks, p.p_depth,
      p.p_inside, p.mid, p.lot, p.p_qty, p.mean_lifetime, p.p_modify_reduce, p.max_reprice_ticks);
}

auto generate(const GenParams& p, GenStats* stats) -> std::vector<Op> { return Generator(p).run(stats); }

}  // namespace lob::workload
