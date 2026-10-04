// ExactDiv (v11 slot_of) against the reference: x is a multiple of d below
// d * n  <=>  quotient(x) < n, and then quotient(x) == x / d.

#include <lob/v11/exact_div.hpp>
#include <lob/workload/generator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

TEST_CASE("ExactDiv matches % and / on multiples, rejects everything else", "[v11][exact_div]") {
  constexpr std::uint64_t kSlots = std::uint64_t{1} << 16;
  lob::workload::Rng rng(11);
  for (const std::uint64_t d : {1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 25ULL, 64ULL, 100ULL, 125ULL, 10'000ULL,
                                (1ULL << 20) + 1, 1'000'000'007ULL}) {
    const lob::v11::ExactDiv div(d);
    auto check = [&](std::int64_t signed_x) {
      const auto x = static_cast<std::uint64_t>(signed_x);
      const bool want = signed_x >= 0 && x % d == 0 && x / d < kSlots;
      const std::uint64_t q = div.quotient(x);
      INFO("d=" << d << " x=" << signed_x << " q=" << q);
      REQUIRE((q < kSlots) == want);
      if (want) REQUIRE(q == x / d);
    };
    // every multiple in range, plus its neighbours (the edge of the window included)
    const std::uint64_t step = kSlots <= 4096 ? 1 : kSlots / 4096;
    for (std::uint64_t s = 0; s <= kSlots; s += step) {
      const auto x = static_cast<std::int64_t>(s * d);
      check(x);
      check(x - 1);
      check(x + 1);
    }
    check(static_cast<std::int64_t>((kSlots - 1) * d));
    check(static_cast<std::int64_t>(kSlots * d));
    // negative differences (price below the window base) and random values
    for (int i = 0; i < 20'000; ++i) {
      const auto r = static_cast<std::int64_t>(rng.uniform(4 * kSlots * d));
      check(r - static_cast<std::int64_t>(2 * kSlots * d));
      check(-static_cast<std::int64_t>(d) * static_cast<std::int64_t>(rng.uniform(kSlots)));
    }
    check(std::numeric_limits<std::int64_t>::min());
    check(std::numeric_limits<std::int64_t>::max());
  }
}
