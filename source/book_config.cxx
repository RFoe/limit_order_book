#include <lob/book_config.hpp>

#include <cstdint>

namespace lob {

auto infer_tick(std::span<const Op> ops) -> Price {
  Price best = 1;
  for (Price t = 10; t <= 1'000'000; t *= 10) {
    std::uint64_t priced = 0;
    std::uint64_t on_grid = 0;
    for (const Op& op : ops) {
      if (op.type == OpType::Cancel) continue;
      ++priced;
      on_grid += op.price % t == 0 ? 1 : 0;
    }
    if (priced == 0 || on_grid * 1000 < priced * 999) break;
    best = t;
  }
  return best;
}

}  // namespace lob
