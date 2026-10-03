// Google Benchmark: replay whole workloads through every registered version.
//
//   benchmark_book --workload=data/a.ops,data/b.ops [google benchmark flags]
//   (or LOB_WORKLOAD=a.ops,b.ops)
//
// One benchmark per (version x workload). Each iteration builds a fresh book
// (untimed), replays all ops (timed) and destroys the book (untimed). Use
// --benchmark_enable_random_interleaving=true so repetitions of different
// versions alternate and share the same noise (scripts/bench.sh does this).

#include <lob/book_config.hpp>
#include <lob/events.hpp>
#include <lob/replay.hpp>
#include <lob/versions.hpp>
#include <lob/workload/format.hpp>

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace lob;

template <class Book>
void bm_replay(benchmark::State& state, std::span<const Op> ops, const BookConfig& cfg) {
  for (auto _ : state) {
    state.PauseTiming();
    ChecksumSink sink;
    auto book = make_book<Book>(sink, cfg);
    state.ResumeTiming();

    replay(*book, ops);
    benchmark::DoNotOptimize(sink.value);

    state.PauseTiming();
    book.reset();
    state.ResumeTiming();
  }
  const auto n = static_cast<std::int64_t>(ops.size());
  state.SetItemsProcessed(state.iterations() * n);
  state.counters["ns_per_op"] = benchmark::Counter(static_cast<double>(state.iterations() * n),
                                                   benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

auto take_workload_flag(int& argc, char** argv) -> std::string {
  std::string value;
  int out = 1;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a.starts_with("--workload=")) {
      value = a.substr(std::string_view("--workload=").size());
    } else {
      argv[out++] = argv[i];
    }
  }
  argc = out;
  if (value.empty())
    if (const char* env = std::getenv("LOB_WORKLOAD")) value = env;
  return value;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string list = take_workload_flag(argc, argv);
  if (list.empty()) {
    std::println(stderr, "benchmark_book: pass --workload=a.ops[,b.ops] or set LOB_WORKLOAD");
    return 2;
  }

  std::deque<workload::Workload> workloads;  // stable addresses for the lambdas
  for (std::string_view rest = list; !rest.empty();) {
    const auto comma = rest.find(',');
    const std::string path(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    auto w = workload::load(path);
    if (!w) {
      std::println(stderr, "benchmark_book: {}", w.error());
      return 1;
    }
    const std::string stem = std::filesystem::path(path).stem().string();
    const auto& stored = workloads.emplace_back(std::move(*w));
    const BookConfig cfg{.tick = infer_tick(stored.ops)};
    for_each_type<Versions::with_sink<ChecksumSink>>([&]<class B>() {
      benchmark::RegisterBenchmark(std::format("replay/{}/{}", stem, B::name),
                                   [&stored, cfg](benchmark::State& st) { bm_replay<B>(st, stored.ops, cfg); })
          ->Unit(benchmark::kMillisecond);
    });
  }

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
