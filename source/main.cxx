// book: command-line driver for the order book lab.
//
//   book gen       --out F [--seed N --ops N --warmup N --p-cancel X ...]
//   book info      F
//   book validate  F                       replay through v0, count events
//   book itch2ops  IN|- --symbols A,B [--out-dir D] [--tag T] [--top N]
//   book replay    --workload F [--version v0] [--repeat N]
//   book latency   --workload F [--versions v0,v1] [--rounds R] [--csv F]
//   book versions
//
// `replay` is the target for cachegrind/perf/heaptrack: under valgrind only the
// replay loop is instrumented (CACHEGRIND_START/STOP_INSTRUMENTATION), so file
// loading does not show up in Ir.

#include <lob/events.hpp>
#include <lob/harness/latency.hpp>
#include <lob/harness/tsc.hpp>
#include <lob/itch/messages.hpp>
#include <lob/itch/parser.hpp>
#include <lob/itch/to_ops.hpp>
#include <lob/replay.hpp>
#include <lob/versions.hpp>
#include <lob/workload/format.hpp>
#include <lob/workload/generator.hpp>

#if __has_include(<valgrind/cachegrind.h>)
#include <valgrind/cachegrind.h>
#define LOB_CG_START CACHEGRIND_START_INSTRUMENTATION
#define LOB_CG_STOP CACHEGRIND_STOP_INSTRUMENTATION
#else
#define LOB_CG_START
#define LOB_CG_STOP
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <numeric>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace lob;

// ---- argument parsing -------------------------------------------------------

struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::string, std::less<>> options;

  [[nodiscard]] auto has(std::string_view k) const -> bool { return options.contains(k); }
  [[nodiscard]] auto str(std::string_view k, std::string def = {}) const -> std::string {
    auto it = options.find(k);
    return it == options.end() ? def : it->second;
  }
  template <class T>
  [[nodiscard]] auto num(std::string_view k, T def) const -> T {
    auto it = options.find(k);
    if (it == options.end()) return def;
    T v{};
    const auto& s = it->second;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size())
      throw std::invalid_argument(std::format("--{}: cannot parse '{}'", k, s));
    return v;
  }
};

auto parse_args(int argc, char** argv, int first) -> Args {
  Args a;
  for (int i = first; i < argc; ++i) {
    std::string_view s = argv[i];
    if (s.starts_with("--")) {
      s.remove_prefix(2);
      if (auto eq = s.find('='); eq != std::string_view::npos) {
        a.options.emplace(std::string(s.substr(0, eq)), std::string(s.substr(eq + 1)));
      } else if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--")) {
        a.options.emplace(std::string(s), argv[++i]);
      } else {
        a.options.emplace(std::string(s), "1");
      }
    } else {
      a.positional.emplace_back(s);
    }
  }
  return a;
}

auto split(std::string_view s, char sep) -> std::vector<std::string> {
  std::vector<std::string> out;
  while (!s.empty()) {
    const auto p = s.find(sep);
    out.emplace_back(s.substr(0, p));
    if (p == std::string_view::npos) break;
    s.remove_prefix(p + 1);
  }
  return out;
}

auto load_or_die(const std::string& path) -> workload::Workload {
  auto w = workload::load(path);
  if (!w) throw std::runtime_error(w.error());
  return std::move(*w);
}

using ChecksumBooks = Versions::with_sink<ChecksumSink>;

auto version_names() -> std::vector<std::string> {
  std::vector<std::string> names;
  for_each_type<ChecksumBooks>([&]<class B>() { names.emplace_back(B::name); });
  return names;
}

// Calls f.template operator()<Book>() for the version named `name`.
template <class F>
void with_version(std::string_view name, F&& f) {
  bool found = false;
  for_each_type<ChecksumBooks>([&]<class B>() {
    if (B::name == name) {
      found = true;
      f.template operator()<B>();
    }
  });
  if (!found) throw std::invalid_argument(std::format("unknown version '{}'", name));
}

// Counts every event type (validation, not the hot path).
struct StatsSink {
  std::uint64_t trades = 0, rested = 0, cancelled = 0, reduced = 0, rejected = 0;
  std::uint64_t traded_qty = 0;
  void on(const Trade& t) {
    ++trades;
    traded_qty += t.qty;
  }
  void on(const Rested&) { ++rested; }
  void on(const Cancelled&) { ++cancelled; }
  void on(const Reduced&) { ++reduced; }
  void on(const Rejected&) { ++rejected; }
};

void print_validation(std::span<const Op> ops) {
  StatsSink s;
  v0::Book<StatsSink> book(s);
  replay(book, ops);
  std::println("  v0 replay: ops={} trades={} traded_qty={} rested={} cancelled={} reduced={} rejected={} "
               "final_orders={} bid_levels={} ask_levels={}",
               ops.size(), s.trades, s.traded_qty, s.rested, s.cancelled, s.reduced, s.rejected, book.order_count(),
               book.levels(Side::Buy).size(), book.levels(Side::Sell).size());
}

void print_op_mix(std::span<const Op> ops) {
  std::array<std::uint64_t, 3> n{};
  for (const Op& op : ops) ++n[static_cast<std::size_t>(op.type)];
  const auto pct = [&](std::uint64_t x) { return ops.empty() ? 0.0 : 100.0 * static_cast<double>(x) / static_cast<double>(ops.size()); };
  std::println("  op mix: push={} ({:.1f}%) cancel={} ({:.1f}%) modify={} ({:.1f}%)", n[0], pct(n[0]), n[1], pct(n[1]),
               n[2], pct(n[2]));
}

// ---- commands -----------------------------------------------------------------

auto cmd_gen(const Args& a) -> int {
  workload::GenParams p;
  p.seed = a.num("seed", p.seed);
  p.n_ops = a.num("ops", p.n_ops);
  p.warmup = a.num("warmup", p.warmup);
  p.p_modify = a.num("p-modify", p.p_modify);
  p.p_buy = a.num("p-buy", p.p_buy);
  p.p_marketable = a.num("p-marketable", p.p_marketable);
  p.max_cross_ticks = a.num("cross-ticks", p.max_cross_ticks);
  p.p_depth = a.num("p-depth", p.p_depth);
  p.p_inside = a.num("p-inside", p.p_inside);
  p.mid = a.num("mid", p.mid);
  p.lot = a.num("lot", p.lot);
  p.p_qty = a.num("p-qty", p.p_qty);
  p.mean_lifetime = a.num("lifetime", p.mean_lifetime);
  p.p_modify_reduce = a.num("p-reduce", p.p_modify_reduce);
  p.max_reprice_ticks = a.num("reprice-ticks", p.max_reprice_ticks);
  const std::string out = a.str("out");
  if (out.empty()) throw std::invalid_argument("gen: --out is required");

  workload::GenStats st;
  const auto ops = workload::generate(p, &st);
  const auto desc = workload::describe(p);
  if (auto r = workload::save(out, workload::Source::Synthetic, p.seed, desc, ops); !r) throw std::runtime_error(r.error());
  std::println("wrote {} ({} ops)\n  {}", out, ops.size(), desc);
  std::println("  pushes={} (marketable={}) cancels={} modifies={} (reduce={} reprice={}) fallbacks={} trades={} "
               "final_orders={}",
               st.pushes, st.marketable, st.cancels, st.modifies, st.reduces, st.reprices, st.fallbacks, st.trades,
               st.final_orders);
  const auto n = static_cast<double>(ops.size());
  std::println("  cancel/op={:.3f} cancel/push={:.3f} trades/op={:.3f}", static_cast<double>(st.cancels) / n,
               static_cast<double>(st.cancels) / static_cast<double>(st.pushes), static_cast<double>(st.trades) / n);
  return 0;
}

auto cmd_info(const Args& a) -> int {
  if (a.positional.empty()) throw std::invalid_argument("info: workload path required");
  for (const auto& path : a.positional) {
    const auto w = load_or_die(path);
    std::println("{}: source={} n_ops={} seed={} checksum={:016x}\n  {}", path, to_string(w.header.source),
                 w.header.n_ops, w.header.seed, w.header.checksum, w.description());
    print_op_mix(w.ops);
  }
  return 0;
}

auto cmd_validate(const Args& a) -> int {
  if (a.positional.empty()) throw std::invalid_argument("validate: workload path required");
  for (const auto& path : a.positional) {
    const auto w = load_or_die(path);
    std::println("{}:", path);
    print_op_mix(w.ops);
    print_validation(w.ops);
  }
  return 0;
}

auto cmd_itch2ops(const Args& a) -> int {
  if (a.positional.empty()) throw std::invalid_argument("itch2ops: input path (or -) required");
  const std::string in = a.positional.front();
  const auto symbols = split(a.str("symbols", "AAPL"), ',');
  const std::filesystem::path out_dir = a.str("out-dir", ".");
  const std::string tag = a.str("tag", "itch");
  const auto top = a.num<std::size_t>("top", 10);

  std::FILE* f = in == "-" ? stdin : std::fopen(in.c_str(), "rb");
  if (!f) throw std::runtime_error(std::format("cannot open {}", in));
  itch::Reader reader(f);
  itch::ToOps conv(symbols);
  std::uint64_t malformed = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::span<const std::byte> msg; reader.next(msg);)
    if (!itch::dispatch(msg, conv)) ++malformed;
  if (f != stdin) std::fclose(f);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::println("parsed {} messages ({:.2f} GB) in {:.1f}s, malformed={}", reader.messages(),
               static_cast<double>(reader.bytes()) / 1e9, secs, malformed);

  std::filesystem::create_directories(out_dir);
  for (std::size_t i = 0; i < conv.symbols().size(); ++i) {
    const auto& sym = conv.symbols()[i];
    const auto& ops = conv.ops(i);
    const auto& st = conv.stats(i);
    const auto path = out_dir / std::format("{}_{}.ops", tag, sym);
    const auto desc = std::format("itch {} symbol={} adds={} exec={} cancel={} delete={} replace={} overfills={}", tag,
                                  sym, st.adds, st.executions, st.cancels, st.deletes, st.replaces, st.overfills);
    if (auto r = workload::save(path, workload::Source::Itch, 0, desc, ops); !r) throw std::runtime_error(r.error());
    std::println("wrote {} ({} ops)\n  {}", path.string(), ops.size(), desc);
    print_op_mix(ops);
    print_validation(ops);
  }

  std::vector<std::uint16_t> locates(65536);
  std::iota(locates.begin(), locates.end(), 0);
  const auto& adds = conv.adds_by_locate();
  std::ranges::sort(locates, [&](auto x, auto y) { return adds[x] > adds[y]; });
  std::println("top {} symbols by add orders:", top);
  for (std::size_t i = 0; i < top && adds[locates[i]] > 0; ++i)
    std::println("  {:<8} adds={}", conv.symbol_of_locate(locates[i]), adds[locates[i]]);
  return 0;
}

auto cmd_replay(const Args& a) -> int {
  const auto w = load_or_die(a.str("workload"));
  const std::string version = a.str("version", "v0");
  const auto repeat = a.num<int>("repeat", 1);
  with_version(version, [&]<class B>() {
    for (int r = 0; r < repeat; ++r) {
      ChecksumSink sink;
      auto book = std::make_unique<B>(sink);
      const auto t0 = std::chrono::steady_clock::now();
      LOB_CG_START;
      replay(*book, std::span<const Op>(w.ops));
      LOB_CG_STOP;
      const auto t1 = std::chrono::steady_clock::now();
      harness::do_not_optimize(sink.value);
      std::println("{} run={} ops={} events={} trades={} checksum={:016x} orders_left={} wall_ms={:.1f}", B::name, r,
                   w.ops.size(), sink.events, sink.trades, sink.value, book->order_count(),
                   std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
  });
  return 0;
}

auto cmd_latency(const Args& a) -> int {
  const auto w = load_or_die(a.str("workload"));
  const std::string vs = a.str("versions", "all");
  const auto versions = vs == "all" ? version_names() : split(vs, ',');
  const auto rounds = a.num<int>("rounds", 5);
  const std::string csv = a.str("csv");

  const auto tsc = harness::probe_tsc();
  std::println("tsc: constant_tsc={} nonstop_tsc={} freq={:.4f} GHz timer_overhead={} cycles", tsc.constant_tsc,
               tsc.nonstop_tsc, tsc.ghz, tsc.overhead);
  if (!tsc.constant_tsc || !tsc.nonstop_tsc) std::println("WARNING: TSC is not invariant; cycle numbers are unreliable");

  // round 0 is warm-up and discarded; versions are interleaved and the order is
  // rotated every round so all of them see the same noise
  std::vector<harness::Samples> samples(versions.size());
  for (int r = 0; r <= rounds; ++r) {
    for (std::size_t k = 0; k < versions.size(); ++k) {
      const std::size_t v = (k + static_cast<std::size_t>(r)) % versions.size();
      harness::Samples s;
      with_version(versions[v], [&]<class B>() { harness::measure_once<B>(w.ops, s); });
      if (r == 0) continue;
      for (std::size_t c = 0; c < harness::kOpClasses; ++c)
        samples[v].by_class[c].insert(samples[v].by_class[c].end(), s.by_class[c].begin(), s.by_class[c].end());
      samples[v].all.insert(samples[v].all.end(), s.all.begin(), s.all.end());
    }
  }

  std::ofstream out;
  if (!csv.empty()) {
    out.open(csv);
    out << std::format("# workload={} rounds={} tsc_ghz={:.4f} timer_overhead_cycles={}\n", a.str("workload"), rounds,
                       tsc.ghz, tsc.overhead);
    out << "version,class,count,p50,p90,p99,p99.9,p99.99,max\n";
  }
  std::println("{:<6} {:<11} {:>10} {:>7} {:>7} {:>7} {:>8} {:>8} {:>9}   (cycles, incl. timer overhead)", "ver",
               "class", "count", "p50", "p90", "p99", "p99.9", "p99.99", "max");
  for (std::size_t v = 0; v < versions.size(); ++v) {
    auto emit = [&](std::string_view cls, std::vector<std::uint32_t>& data) {
      const auto p = harness::percentiles(std::move(data));
      if (p.count == 0) return;
      std::println("{:<6} {:<11} {:>10} {:>7} {:>7} {:>7} {:>8} {:>8} {:>9}", versions[v], cls, p.count, p.p50, p.p90,
                   p.p99, p.p999, p.p9999, p.max);
      if (out.is_open())
        out << std::format("{},{},{},{},{},{},{},{},{}\n", versions[v], cls, p.count, p.p50, p.p90, p.p99, p.p999,
                           p.p9999, p.max);
    };
    emit("all", samples[v].all);
    for (std::size_t c = 0; c < harness::kOpClasses; ++c) emit(harness::kOpClassNames[c], samples[v].by_class[c]);
  }
  if (out.is_open()) std::println("wrote {}", csv);
  return 0;
}

auto cmd_versions(const Args&) -> int {
  for (const auto& n : version_names()) std::println("{}", n);
  return 0;
}

void usage() {
  std::println(stderr,
               "usage: book <gen|info|validate|itch2ops|replay|latency|versions> [args]\n"
               "  see the header comment of source/main.cxx for options");
}

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string_view cmd = argv[1];
  try {
    const Args args = parse_args(argc, argv, 2);
    if (cmd == "gen") return cmd_gen(args);
    if (cmd == "info") return cmd_info(args);
    if (cmd == "validate") return cmd_validate(args);
    if (cmd == "itch2ops") return cmd_itch2ops(args);
    if (cmd == "replay") return cmd_replay(args);
    if (cmd == "latency") return cmd_latency(args);
    if (cmd == "versions") return cmd_versions(args);
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::println(stderr, "error: {}", e.what());
    return 1;
  }
}
