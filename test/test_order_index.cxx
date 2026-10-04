// v15 OrderIndex against std::unordered_map: random insert-if-absent / find /
// erase / value updates, with ids in the 32-bit key range (dense and small),
// at its edge (2^32 - 2 is a key, 2^32 - 1 is the empty marker and must go to
// the fallback map), >= 2^32 (fallback), duplicates, and enough entries for
// growth and full buckets (overflow chains); the second half draws mostly
// increasing ids, the case v16's insert handles without reading the bucket.

#include <lob/v15/order_index.hpp>
#include <lob/v16/order_index.hpp>
#include <lob/workload/generator.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

TEMPLATE_TEST_CASE("OrderIndex matches std::unordered_map", "[v15][v16][order_index]", lob::v15::OrderIndex,
                   lob::v16::OrderIndex) {
  TestType ix;
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  std::vector<std::uint64_t> keys;  // ids ever inserted (some erased since)
  lob::workload::Rng rng(15);
  const std::uint64_t first = 3'000'000'000ULL;  // near the top of the 32-bit key range
  std::uint32_t next_value = 1;

  std::uint64_t mono = first;  // second half: mostly increasing ids (the v16 fast path)
  int step_no = 0;
  auto draw_id = [&]() -> std::uint64_t {
    if (step_no >= 200'000) {
      const auto r = rng.uniform(100);
      if (r < 85) return mono += 1 + rng.uniform(40);                          // new, increasing
      if (r < 95) return keys.empty() ? mono : keys[rng.uniform(keys.size())];  // existing / erased
      return mono - rng.uniform(5000);                                         // a little back
    }
    const auto r = rng.uniform(100);
    if (r < 60) return first + rng.uniform(1'000'000);             // dense, inside the window
    if (r < 75) return first - rng.uniform(1'000'000);             // below the first id, still inside
    if (r < 80) return rng.uniform(1'000'000);                     // small ids
    if (r < 84) return (1ULL << 33) + rng.uniform(1000);           // >= 2^32: fallback
    if (r < 85) return 0xFFFF'FFFEULL + rng.uniform(3);            // the 32-bit edge
    return keys.empty() ? first : keys[rng.uniform(keys.size())];  // an existing (or erased) id
  };

  for (int step = 0; step < 400'000; ++step) {
    step_no = step;
    const std::uint64_t id = draw_id();
    const auto op = rng.uniform(10);
    if (op < 5) {  // insert-if-absent
      bool inserted = false;
      auto h = ix.try_insert(id, inserted);
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
        ix.erase(h, id);
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
    const std::uint32_t* p = static_cast<const TestType&>(ix).find(id);
    REQUIRE(p != nullptr);
    REQUIRE(*p == v);
  }
  for (const auto& [id, v] : ref) ix.erase(ix.find(id), id);
  CHECK(ix.size() == 0);
  CHECK_FALSE(ix.find(first));
}
