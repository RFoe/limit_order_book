#include <lob/harness/latency.hpp>

#include <algorithm>
#include <cmath>

namespace lob::harness {

auto percentiles(std::vector<std::uint32_t> v) -> Percentiles {
  Percentiles p;
  p.count = v.size();
  if (v.empty()) return p;
  std::ranges::sort(v);
  // nearest-rank
  auto at = [&](double q) -> std::uint64_t {
    const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(v.size())));
    return v[std::clamp<std::size_t>(rank, 1, v.size()) - 1];
  };
  p.p50 = at(0.50);
  p.p90 = at(0.90);
  p.p99 = at(0.99);
  p.p999 = at(0.999);
  p.p9999 = at(0.9999);
  p.max = v.back();
  return p;
}

}  // namespace lob::harness
