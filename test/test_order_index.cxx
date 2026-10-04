// v15 OrderIndex against std::unordered_map: random insert-if-absent / find /
// erase / value updates, with ids around the first one (32-bit window, below
// and above it), far away (fallback map), duplicates, and enough entries for
// growth and full buckets (overflow chains).

#include <lob/v15/order_index.hpp>
#include <lob/workload/generator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

TEST_CASE("OrderIndex matches std::unordered_map", "[v15][order_index]") {
  lob::v15::OrderIndex ix;
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  std::vector<std::uint64_t> keys;  // ids ever inserted (some erased since)
  lob::workload::Rng rng(15);
  const std::uint64_t first = 5'000'000'000ULL;
  std::uint32_t next_value = 1;

  auto draw_id = [&]() -> std::uint64_t {
    const auto r = rng.uniform(100);
    if (r < 60) return first + rng.uniform(1'000'000);             // dense, inside the window
    if (r < 75) return first - rng.uniform(1'000'000);             // below the first id, still inside
    if (r < 80) return rng.uniform(1'000'000);                     // far below: fallback
    if (r < 85) return first + (1ULL << 33) + rng.uniform(1000);   // far above: fallback
    return keys.empty() ? first : keys[rng.uniform(keys.size())];  // an existing (or erased) id
  };

  for (int step = 0; step < 400'000; ++step) {
    const std::uint64_t id = draw_id();
    const auto op = rng.uniform(10);
    if (op < 5) {  // insert-if-absent
      auto [h, inserted] = ix.try_insert(id);
      REQUIRE(static_cast<bool>(h));
      REQUIRE(inserted == !ref.contains(id));
      if (inserted) {
        *h.value = next_value;
        ref[id] = next_value++;
        keys.push_back(id);
      } else {
        REQUIRE(*h.value == ref.at(id));
      }
    } else if (op < 8) {  // find + erase
      auto h = ix.find(id);
      REQUIRE(static_cast<bool>(h) == ref.contains(id));
      if (h) {
        REQUIRE(*h.value == ref.at(id));
        ix.erase(h);
        ref.erase(id);
      }
    } else {  // find + update
      auto h = ix.find(id);
      REQUIRE(static_cast<bool>(h) == ref.contains(id));
      if (h) {
        *h.value = next_value;
        ref[id] = next_value++;
      }
    }
    REQUIRE(ix.size() == ref.size());
  }
  // everything left is still found with its value; erase all of it
  for (const auto& [id, v] : ref) {
    const std::uint32_t* p = static_cast<const lob::v15::OrderIndex&>(ix).find(id);
    REQUIRE(p != nullptr);
    REQUIRE(*p == v);
  }
  for (const auto& [id, v] : ref) ix.erase(ix.find(id));
  CHECK(ix.size() == 0);
  CHECK_FALSE(ix.find(first));
}
