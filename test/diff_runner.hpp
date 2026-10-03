#pragma once
// Differential testing: feed the same op stream to a reference book (v0) and a
// candidate, compare the event sequence op by op, check invariants after every
// op, and compare the final book state level by level. On failure, shrink the op
// stream to a short reproduction.

#include <lob/book_concept.hpp>
#include <lob/book_config.hpp>
#include <lob/events.hpp>
#include <lob/replay.hpp>
#include <lob/types.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lob::testing {

struct DiffFailure {
  std::size_t op_index;  // == ops.size() for an end-state mismatch
  std::string detail;
};

inline auto format_events(std::span<const Event> events) -> std::string {
  std::string s;
  for (const auto& e : events) s += std::format("\n      {}", to_string(e));
  return events.empty() ? std::string("\n      (none)") : s;
}

inline auto format_levels(const std::vector<LevelSnapshot>& levels) -> std::string {
  std::string s;
  for (const auto& l : levels) s += std::format(" {}x{}({})", l.price, l.total_qty, l.order_count);
  return s;
}

// Ref and Cand are book types instantiated with RecordingSink.
template <class Ref, class Cand>
auto run_diff(std::span<const Op> ops, const BookConfig& cfg = {}) -> std::optional<DiffFailure> {
  RecordingSink ref_sink;
  RecordingSink cand_sink;
  const auto ref_book = make_book<Ref>(ref_sink, cfg);
  const auto cand_book = make_book<Cand>(cand_sink, cfg);
  Ref& ref = *ref_book;
  Cand& cand = *cand_book;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    try {
      apply(ref, ops[i]);
      apply(cand, ops[i]);
      ref.check_invariants();
      cand.check_invariants();
    } catch (const std::exception& e) {
      return DiffFailure{i, std::format("op #{} {}: exception: {}", i, to_string(ops[i]), e.what())};
    }
    if (ref_sink.events != cand_sink.events)
      return DiffFailure{i, std::format("op #{} {}: events differ\n    expected ({}):{}\n    actual ({}):{}", i,
                                        to_string(ops[i]), Ref::name, format_events(ref_sink.events), Cand::name,
                                        format_events(cand_sink.events))};
    ref_sink.events.clear();
    cand_sink.events.clear();
  }
  for (Side side : {Side::Buy, Side::Sell}) {
    const auto rl = ref.levels(side);
    const auto cl = cand.levels(side);
    if (rl != cl)
      return DiffFailure{ops.size(), std::format("end state: {} levels differ\n    expected:{}\n    actual:  {}",
                                                 to_string(side), format_levels(rl), format_levels(cl))};
  }
  if (ref.order_count() != cand.order_count())
    return DiffFailure{ops.size(),
                       std::format("end state: order_count {} != {}", ref.order_count(), cand.order_count())};
  return std::nullopt;
}

// Shrinks a failing op stream: cut everything after the first divergence, then
// greedily delete chunks (n/2, n/4, ..., 1) as long as the run still fails.
// Deleting ops can turn later ops into rejects; that is fine, both books must
// still agree on them.
template <class Ref, class Cand>
auto shrink(std::vector<Op> ops, const BookConfig& cfg = {}, std::size_t max_runs = 2'000) -> std::vector<Op> {
  auto first = run_diff<Ref, Cand>(ops, cfg);
  if (!first) return ops;
  auto truncate = [&](std::size_t idx) {
    if (idx < ops.size()) ops.resize(idx + 1);
  };
  truncate(first->op_index);
  std::size_t runs = 1;
  for (std::size_t chunk = std::max<std::size_t>(ops.size() / 2, 1);; chunk /= 2) {
    for (std::size_t i = 0; i < ops.size() && runs < max_runs;) {
      std::vector<Op> candidate;
      candidate.reserve(ops.size());
      candidate.insert(candidate.end(), ops.begin(), ops.begin() + static_cast<std::ptrdiff_t>(i));
      candidate.insert(candidate.end(), ops.begin() + static_cast<std::ptrdiff_t>(std::min(i + chunk, ops.size())),
                       ops.end());
      ++runs;
      if (auto f = run_diff<Ref, Cand>(candidate, cfg)) {
        ops = std::move(candidate);
        truncate(f->op_index);
      } else {
        i += chunk;
      }
    }
    if (chunk <= 1 || runs >= max_runs) break;
  }
  return ops;
}

inline auto report(std::uint64_t seed, const DiffFailure& f, std::span<const Op> minimal,
                   const std::optional<DiffFailure>& minimal_failure) -> std::string {
  std::string s = std::format("differential test failed: seed={}\n  {}\n  minimal reproduction ({} ops):", seed,
                              f.detail, minimal.size());
  for (std::size_t i = 0; i < minimal.size(); ++i) s += std::format("\n    #{} {}", i, to_string(minimal[i]));
  if (minimal_failure) s += std::format("\n  minimal failure: {}", minimal_failure->detail);
  return s;
}

}  // namespace lob::testing
