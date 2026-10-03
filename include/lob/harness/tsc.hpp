#pragma once
// TSC-based timing for the latency harness.
//   begin: lfence; rdtsc; lfence   -> earlier instructions retired, later ones not started
//   end:   rdtscp; lfence          -> measured code retired before the read
// Only meaningful with constant_tsc + nonstop_tsc (checked at startup).

#include <x86intrin.h>

#include <cstdint>

namespace lob::harness {

inline auto tsc_begin() noexcept -> std::uint64_t {
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

inline auto tsc_end() noexcept -> std::uint64_t {
  unsigned aux = 0;
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}

template <class T>
inline void do_not_optimize(const T& value) noexcept {
  asm volatile("" : : "r,m"(value) : "memory");
}

struct TscInfo {
  bool constant_tsc = false;
  bool nonstop_tsc = false;
  double ghz = 0;                // calibrated against CLOCK_MONOTONIC_RAW
  std::uint64_t overhead = 0;    // median cycles of an empty begin/end pair
};

// Reads /proc/cpuinfo flags, calibrates the TSC (median of 3 x 100 ms) and
// measures the timer overhead.
auto probe_tsc() -> TscInfo;

}  // namespace lob::harness
