#include <lob/events.hpp>
#include <lob/replay.hpp>
#include <lob/v0/book.hpp>
#include <lob/workload/format.hpp>
#include <lob/workload/generator.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>

namespace {

using namespace lob;

auto small_params() -> workload::GenParams {
  workload::GenParams p;
  p.seed = 123;
  p.n_ops = 50'000;
  p.warmup = 1'000;
  return p;
}

auto temp_path(const char* name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / std::format("lob_test_{}_{}", ::getpid(), name);
}

}  // namespace

TEST_CASE("generator is deterministic for a seed", "[workload]") {
  const auto a = workload::generate(small_params());
  const auto b = workload::generate(small_params());
  REQUIRE(a.size() == 50'000);
  CHECK(a == b);
  CHECK(workload::checksum(a) == workload::checksum(b));

  auto other = small_params();
  other.seed = 124;
  CHECK(workload::generate(other) != a);
}

TEST_CASE("generator reaches a stationary, cancel-heavy book", "[workload]") {
  auto p = small_params();
  p.n_ops = 200'000;
  workload::GenStats st;
  const auto ops = workload::generate(p, &st);
  const auto n = static_cast<double>(p.n_ops);
  CHECK(st.fallbacks == 0);
  CHECK(st.marketable > 0);
  CHECK(st.trades > 0);
  // most orders die by cancellation, like real order flow
  CHECK(static_cast<double>(st.cancels) / static_cast<double>(st.pushes) > 0.8);
  // modify is drawn on post-warm-up steps without an expired order
  const auto modify_steps = static_cast<double>(p.n_ops - p.warmup - st.cancels);
  CHECK(static_cast<double>(st.modifies) / modify_steps == Catch::Approx(p.p_modify).margin(0.01));
  // Little's law: depth ~ rest rate * mean lifetime (loose bound: fills and
  // reprices also remove orders)
  const double rest_rate = static_cast<double>(st.pushes - st.marketable) / n;
  CHECK(static_cast<double>(st.final_orders) > 0.5 * rest_rate * p.mean_lifetime);
  CHECK(static_cast<double>(st.final_orders) < 1.5 * rest_rate * p.mean_lifetime);

  // every generated op is valid against the reference book (generate() also
  // throws on rejects); apply without per-step checks to keep the debug run fast
  ChecksumSink sink;
  v0::Book<ChecksumSink> book(sink);
  for (const Op& op : ops) apply(book, op);
  book.check_invariants();
  CHECK(book.order_count() == st.final_orders);
}

TEST_CASE("workload files round-trip and detect corruption", "[workload]") {
  const auto ops = workload::generate(small_params());
  const auto path = temp_path("roundtrip.ops");
  REQUIRE(workload::save(path, workload::Source::Synthetic, 123, "unit test", ops).has_value());

  auto w = workload::load(path);
  REQUIRE(w.has_value());
  CHECK(w->ops == ops);
  CHECK(w->header.seed == 123);
  CHECK(w->header.source == workload::Source::Synthetic);
  CHECK(w->description() == "unit test");

  {  // flip one byte in the op payload
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(sizeof(workload::FileHeader) + 100));
    f.put('\x7f');
  }
  CHECK_FALSE(workload::load(path).has_value());
  std::filesystem::remove(path);
  CHECK_FALSE(workload::load(path).has_value());
}

TEST_CASE("rng distributions have the expected means", "[workload]") {
  workload::Rng rng(1);
  const int n = 200'000;
  double geo = 0;
  double expo = 0;
  int heads = 0;
  for (int i = 0; i < n; ++i) {
    geo += rng.geometric(0.25);
    expo += rng.exponential(100.0);
    heads += rng.bernoulli(0.3) ? 1 : 0;
  }
  CHECK(geo / n == Catch::Approx(3.0).epsilon(0.02));  // (1-p)/p
  CHECK(expo / n == Catch::Approx(100.0).epsilon(0.02));
  CHECK(static_cast<double>(heads) / n == Catch::Approx(0.3).epsilon(0.02));
}
