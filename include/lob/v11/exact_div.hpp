#pragma once
// Exact division by a runtime divisor without a div instruction.
//
// Write d = 2^k * m (m odd) and let inv = m^-1 mod 2^64. For any 64-bit x,
//   q(x) = rotr(x * inv, k)
// is a bijection on 64-bit integers, and it maps the multiples of d below
// 2^64 onto 0, 1, 2, ... in order: q(d * s) = s. Every other x (not a multiple
// of d, or a "negative" difference wrapped to unsigned) therefore lands at or
// above ceil(2^64 / d). So for a bound n <= 2^64 / d:
//   q(x) < n  <=>  x == d * s for some s < n,  and then q(x) == s.
// One multiply and one rotate replace the sign check, the remainder check and
// the division (the trick compilers use for `x % c == 0` with a constant c).

#include <bit>
#include <cstdint>
#include <stdexcept>

namespace lob::v11 {

class ExactDiv {
 public:
  ExactDiv() = default;
  explicit ExactDiv(std::uint64_t d) {
    if (d == 0) throw std::invalid_argument("ExactDiv: divisor 0");
    shift_ = static_cast<unsigned>(std::countr_zero(d));
    const std::uint64_t m = d >> shift_;
    std::uint64_t inv = m;  // correct to 3 bits for odd m; each Newton step doubles that
    for (int i = 0; i < 5; ++i) inv *= 2 - m * inv;
    inv_ = inv;
  }

  // x / d if x is a multiple of d, otherwise some value >= ceil(2^64 / d)
  [[nodiscard]] auto quotient(std::uint64_t x) const noexcept -> std::uint64_t {
    // the builtin is one funnel shift; std::rotr takes an int count and keeps its
    // "% 64 / zero / negative" handling in the IR the inliner prices, which pushed
    // v11's erase() (cost 565) over the threshold v10's (515) stayed under
    return __builtin_rotateright64(x * inv_, shift_);
  }

 private:
  std::uint64_t inv_ = 1;
  unsigned shift_ = 0;
};

}  // namespace lob::v11
