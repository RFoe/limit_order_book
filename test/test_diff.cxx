// Differential tests: every registered version vs v0 on random workloads, plus a
// deliberately broken "mutant" that proves the framework catches bugs and
// shrinks them to a short reproduction.
//
// Env overrides: LOB_DIFF_SEEDS=1,2,3  LOB_DIFF_OPS=50000
//                LOB_DIFF_VERSIONS=v7[,v8]  only these versions (unset or "all":
//                every version). Validating a new vN only needs vN: older versions
//                did not change. Run all after touching shared code (types,
//                events, harness, generator, book_config, hier_bitmap, ...).

#include "diff_runner.hpp"

#include <lob/v0/book.hpp>
#include <lob/v3/book.hpp>
#include <lob/v4/book.hpp>
#include <lob/v5/book.hpp>
#include <lob/v7/book.hpp>
#include <lob/v8/book.hpp>
#include <lob/versions.hpp>
#include <lob/workload/generator.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {

using namespace lob;
using Ref = v0::Book<RecordingSink>;

// is version `name` selected by LOB_DIFF_VERSIONS (comma list; unset/"all" = every version)
auto selected(std::string_view name) -> bool {
  const char* env = std::getenv("LOB_DIFF_VERSIONS");
  if (env == nullptr || std::string_view(env).empty() || std::string_view(env) == "all") return true;
  for (std::string_view s = env; !s.empty();) {
    const auto comma = s.find(',');
    if (s.substr(0, comma) == name) return true;
    s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
  }
  return false;
}

auto env_u64_list(const char* name, std::vector<std::uint64_t> def) -> std::vector<std::uint64_t> {
  const char* env = std::getenv(name);
  if (env == nullptr) return def;
  std::vector<std::uint64_t> out;
  for (std::string_view s = env; !s.empty();) {
    const auto comma = s.find(',');
    const auto tok = s.substr(0, comma);
    std::uint64_t v = 0;
    std::from_chars(tok.data(), tok.data() + tok.size(), v);
    out.push_back(v);
    s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
  }
  return out;
}

// Rotate through a few generator profiles so matching, deep books and churn
// all get exercised.
auto params_for(std::uint64_t seed, std::uint64_t n_ops) -> workload::GenParams {
  workload::GenParams p;
  p.seed = seed;
  p.n_ops = n_ops;
  p.warmup = 300;
  // short lifetimes keep the book a few hundred orders deep: per-step
  // invariant checks are O(depth) and run under ASan in Debug
  p.mean_lifetime = 400;
  switch (seed % 3) {
    case 0:  // default market-like mix
      break;
    case 1:  // aggressive: lots of crossing orders, multi-level sweeps
      p.p_marketable = 0.30;
      p.max_cross_ticks = 10;
      p.p_qty = 0.2;
      break;
    case 2:  // thin and churny: short lifetimes, many reprices
      p.p_depth = 0.8;
      p.mean_lifetime = 50;
      p.p_modify = 0.3;
      p.p_modify_reduce = 0.3;
      break;
  }
  return p;
}

template <class Sink>
class MutantReduceLosesPriority : public v0::Book<Sink> {
  using Base = v0::Book<Sink>;

 public:
  static constexpr std::string_view name = "mutant";
  using Base::Base;

  // BUG (on purpose): a quantity reduction is implemented as cancel + re-push
  void modify(OrderId id, Price price, Qty qty) {
    if (auto o = Base::find(id); o && qty > 0 && price == o->price && qty < o->qty) {
      Base::cancel(id);
      Base::push(id, o->side, price, qty);
      return;
    }
    Base::modify(id, price, qty);
  }
};

}  // namespace

TEMPLATE_LIST_TEST_CASE("version matches v0 on random workloads", "[diff]",
                        Versions::with_sink<RecordingSink>) {
  if (!selected(TestType::name)) SKIP("version not selected by LOB_DIFF_VERSIONS");
  const auto seeds = env_u64_list("LOB_DIFF_SEEDS", {1, 2, 3, 4, 5, 6});
  const auto n_ops = env_u64_list("LOB_DIFF_OPS", {20'000}).front();
  for (const auto seed : seeds) {
    const auto ops = workload::generate(params_for(seed, n_ops));
    if (auto f = testing::run_diff<Ref, TestType>(ops)) {
      const auto minimal = testing::shrink<Ref, TestType>(ops);
      FAIL(testing::report(seed, *f, minimal, testing::run_diff<Ref, TestType>(minimal)));
    }
  }
}

TEST_CASE("differential framework catches a mutant and shrinks it", "[diff][meta]") {
  const std::uint64_t seed = 7;
  const auto ops = workload::generate(params_for(seed, 3'000));
  const auto f = testing::run_diff<Ref, MutantReduceLosesPriority<RecordingSink>>(ops);
  REQUIRE(f.has_value());
  const auto minimal = testing::shrink<Ref, MutantReduceLosesPriority<RecordingSink>>(ops);
  const auto mf = testing::run_diff<Ref, MutantReduceLosesPriority<RecordingSink>>(minimal);
  INFO(testing::report(seed, *f, minimal, mf));
  REQUIRE(mf.has_value());
  // push X, then a priority-keeping reduce of X: the shortest possible repro
  CHECK(minimal.size() == 2);
  CHECK(minimal.front().type == OpType::Push);
  CHECK(minimal.back().type == OpType::Modify);
}

// v3 keeps two level stores (tick grid + fallback std::map). With the default
// 2^16-slot window and tick 1 every synthetic price lands on the grid, so the
// generic test above never touches the fallback. Here a tiny window and a
// coarser tick force both stores to be live at the same time, including best
// prices that alternate between them during matching.
TEST_CASE("grid + fallback map match v0 (v3 and later)", "[diff][grid]") {
  struct Variant {
    const char* label;
    Price tick;
    bool small_window;  // 2^6 slots instead of 2^16
  };
  const Variant variants[] = {
      {"window 2^6, tick 1", 1, true},
      {"window 2^6, tick 2", 2, true},
      {"window 2^6, tick 3", 3, true},
      {"window 2^16, tick 2", 2, false},
  };
  const auto seeds = env_u64_list("LOB_DIFF_SEEDS", {1, 2, 3, 4, 5, 6});
  const auto n_ops = env_u64_list("LOB_DIFF_OPS", {20'000}).front();
  for (const auto& v : variants) {
    for (const auto seed : seeds) {
      const auto ops = workload::generate(params_for(seed, n_ops));
      const BookConfig cfg{.tick = v.tick};
      if (v.tick > 1) {  // sanity: a large share of prices really is off the grid
        std::size_t priced = 0, off = 0;
        for (const Op& op : ops)
          if (op.type != OpType::Cancel) {
            ++priced;
            off += op.price % v.tick != 0 ? 1 : 0;
          }
        REQUIRE(off * 4 > priced);
      }
      auto check = [&]<class Cand>() {
        if (!selected(Cand::name)) return;
        if (auto f = testing::run_diff<Ref, Cand>(ops, cfg)) {
          const auto minimal = testing::shrink<Ref, Cand>(ops, cfg);
          FAIL(v.label << "\n" << testing::report(seed, *f, minimal, testing::run_diff<Ref, Cand>(minimal, cfg)));
        }
      };
      if (v.small_window) {
        check.template operator()<v3::BookT<RecordingSink, 6>>();
        check.template operator()<v4::BookT<RecordingSink, 6>>();
        check.template operator()<v5::BookT<RecordingSink, 6>>();
        check.template operator()<v7::BookT<RecordingSink, 6>>();
        check.template operator()<v8::BookT<RecordingSink, 6>>();
      } else {
        check.template operator()<v3::BookT<RecordingSink, 16>>();
        check.template operator()<v4::BookT<RecordingSink, 16>>();
        check.template operator()<v5::BookT<RecordingSink, 16>>();
        check.template operator()<v7::BookT<RecordingSink, 16>>();
        check.template operator()<v8::BookT<RecordingSink, 16>>();
      }
    }
  }
}

// v5 keeps two order indexes: 32-bit keys (id - base) and a 64-bit fallback for
// ids outside [base, base + 2^32). Generated ids are small and dense, so the
// generic test only exercises the compact one. Remapping the ids (injectively)
// puts many of them out of range, including ids that alternate between the two
// indexes and ids far below/above the anchor.
TEST_CASE("v5 compact + wide order index match v0", "[diff][v5]") {
  if (!selected("v5")) SKIP("v5 not selected by LOB_DIFF_VERSIONS");
  struct Remap {
    const char* label;
    OrderId (*f)(OrderId);
  };
  const Remap remaps[] = {
      {"even ids + 2^33", [](OrderId id) -> OrderId { return id % 2 == 0 ? id + (OrderId{1} << 33) : id; }},
      {"id << 31", [](OrderId id) -> OrderId { return id << 31; }},
      {"2^62 - id * 2^20", [](OrderId id) -> OrderId { return (OrderId{1} << 62) - (id << 20); }},
  };
  const auto seeds = env_u64_list("LOB_DIFF_SEEDS", {1, 2, 3, 4, 5, 6});
  const auto n_ops = env_u64_list("LOB_DIFF_OPS", {20'000}).front();
  for (const auto& r : remaps) {
    for (const auto seed : seeds) {
      auto ops = workload::generate(params_for(seed, n_ops));
      for (Op& op : ops) op.id = r.f(op.id);
      using Cand = v5::Book<RecordingSink>;
      if (auto f = testing::run_diff<Ref, Cand>(ops)) {
        const auto minimal = testing::shrink<Ref, Cand>(ops);
        FAIL(r.label << "\n" << testing::report(seed, *f, minimal, testing::run_diff<Ref, Cand>(minimal)));
      }
    }
  }
}

