#pragma once
// Synthetic order-flow generator.
//
// Model: a v0 book runs inside the generator as a shadow book, so every op is
// placed relative to the *current* best prices and cancels/modifies always
// target orders that are still live.
//  - push: buy with p_buy; with p_marketable it is priced 0..max_cross_ticks
//    through the opposite best (takes liquidity); otherwise passive at
//    Geometric(p_depth) ticks behind the same-side best (concentrated on the
//    first levels), or one tick inside the spread with p_inside.
//  - cancel: every order draws an exponential lifetime at birth (mean_lifetime
//    ops); once it expires and is still on the book it is cancelled. This is an
//    M/M/inf queue: by Little's law the book holds ~ rest_rate * mean_lifetime
//    orders and is stationary. The cancel ratio is therefore an *output*
//    (~ adds - fills, i.e. high, as in real markets), steered by mean_lifetime
//    and p_marketable. A fixed cancel probability cannot work: unless it
//    exactly equals adds - fills the book either drains or grows without bound
//    (measured: p_cancel=0.42 -> 37 orders left, 0.40 -> 35809 after 1M ops).
//  - modify (p_modify, on steps without an expired order): random live order;
//    with p_modify_reduce a qty reduction (keeps priority), otherwise a reprice
//    by 1..max_reprice_ticks with a new qty (loses priority).
// Determinism: own xoshiro256** PRNG and own distributions (std::*_distribution
// is implementation-defined and differs between libc++ and libstdc++).

#include <lob/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lob::workload {

struct GenParams {
  std::uint64_t seed = 42;
  std::uint64_t n_ops = 1'000'000;
  std::uint64_t warmup = 2'000;  // leading passive pushes that build depth
  double p_modify = 0.10;
  double p_buy = 0.5;
  double p_marketable = 0.05;
  std::uint32_t max_cross_ticks = 3;
  double p_depth = 0.35;
  double p_inside = 0.05;
  Price mid = 100'000;
  Qty lot = 100;
  double p_qty = 0.5;  // qty = lot * (1 + Geometric(p_qty))
  double mean_lifetime = 4'000;  // ops; sets the stationary depth (Little's law)
  double p_modify_reduce = 0.6;
  std::uint32_t max_reprice_ticks = 3;
};

struct GenStats {
  std::uint64_t pushes = 0;
  std::uint64_t marketable = 0;
  std::uint64_t cancels = 0;
  std::uint64_t modifies = 0;
  std::uint64_t reduces = 0;
  std::uint64_t reprices = 0;
  std::uint64_t fallbacks = 0;  // modify turned into push: no live order
  std::uint64_t trades = 0;
  std::uint64_t final_orders = 0;
};

auto describe(const GenParams& p) -> std::string;

auto generate(const GenParams& p, GenStats* stats = nullptr) -> std::vector<Op>;

// xoshiro256** seeded through splitmix64; exposed for tests
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept;
  auto next() noexcept -> std::uint64_t;
  auto uniform(std::uint64_t n) noexcept -> std::uint64_t;  // [0, n)
  auto bernoulli(double p) noexcept -> bool;
  auto geometric(double p) noexcept -> std::uint32_t;  // failures before first success
  auto exponential(double mean) noexcept -> double;

 private:
  std::uint64_t s_[4];
};

}  // namespace lob::workload
