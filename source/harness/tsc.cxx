#include <lob/harness/tsc.hpp>

#include <algorithm>
#include <array>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace lob::harness {

namespace {

auto now_ns() -> std::uint64_t {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

auto calibrate_once(std::uint64_t window_ns) -> double {
  const std::uint64_t n0 = now_ns();
  const std::uint64_t t0 = tsc_begin();
  std::uint64_t n1 = n0;
  while (n1 - n0 < window_ns) n1 = now_ns();
  const std::uint64_t t1 = tsc_end();
  return static_cast<double>(t1 - t0) / static_cast<double>(n1 - n0);
}

}  // namespace

auto probe_tsc() -> TscInfo {
  TscInfo info;
  std::ifstream cpuinfo("/proc/cpuinfo");
  for (std::string line; std::getline(cpuinfo, line);) {
    if (!line.starts_with("flags")) continue;
    std::istringstream words(line);
    for (std::string w; words >> w;) {
      info.constant_tsc |= w == "constant_tsc";
      info.nonstop_tsc |= w == "nonstop_tsc";
    }
    break;
  }

  std::array<double, 3> ghz{};
  for (double& g : ghz) g = calibrate_once(100'000'000);
  std::ranges::sort(ghz);
  info.ghz = ghz[1];

  std::vector<std::uint64_t> empty(10'000);
  for (auto& e : empty) {
    const std::uint64_t t0 = tsc_begin();
    const std::uint64_t t1 = tsc_end();
    e = t1 - t0;
  }
  std::ranges::nth_element(empty, empty.begin() + static_cast<std::ptrdiff_t>(empty.size() / 2));
  info.overhead = empty[empty.size() / 2];
  return info;
}

}  // namespace lob::harness
