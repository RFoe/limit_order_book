#pragma once
// Per-operation latency distribution. Each op is timed individually with the
// TSC; samples are stored in preallocated arrays (no allocation in the loop) and
// bucketed by op class afterwards. Numbers are raw cycles including the timer
// overhead (reported separately). In a VM treat them as relative only.

#include <lob/events.hpp>
#include <lob/harness/tsc.hpp>
#include <lob/replay.hpp>
#include <lob/types.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lob::harness {

enum class OpClass : std::uint8_t { Push = 0, PushTrade = 1, Cancel = 2, Modify = 3 };
inline constexpr std::size_t kOpClasses = 4;
inline constexpr std::array<std::string_view, kOpClasses> kOpClassNames = {"push_rest", "push_trade", "cancel",
                                                                             "modify"};

struct Samples {
  std::array<std::vector<std::uint32_t>, kOpClasses> by_class;
  std::vector<std::uint32_t> all;
};

struct Percentiles {
  std::uint64_t count = 0;
  std::uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
};

auto percentiles(std::vector<std::uint32_t> v) -> Percentiles;

// One timed pass of `ops` through a fresh Book; appends to `out`.
template <class Book>
void measure_once(std::span<const Op> ops, Samples& out) {
  std::vector<std::uint32_t> cycles(ops.size());
  std::vector<std::uint8_t> cls(ops.size());
  ChecksumSink sink;
  auto book = std::make_unique<Book>(sink);
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const std::uint64_t trades_before = sink.trades;
    const std::uint64_t t0 = tsc_begin();
    apply(*book, ops[i]);
    const std::uint64_t t1 = tsc_end();
    const std::uint64_t dt = t1 - t0;
    cycles[i] = dt > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(dt);
    OpClass c{};
    switch (ops[i].type) {
      case OpType::Push: c = sink.trades != trades_before ? OpClass::PushTrade : OpClass::Push; break;
      case OpType::Cancel: c = OpClass::Cancel; break;
      case OpType::Modify: c = OpClass::Modify; break;
    }
    cls[i] = static_cast<std::uint8_t>(c);
  }
  do_not_optimize(sink.value);
  for (std::size_t i = 0; i < ops.size(); ++i) {
    out.by_class[cls[i]].push_back(cycles[i]);
    out.all.push_back(cycles[i]);
  }
}

}  // namespace lob::harness
