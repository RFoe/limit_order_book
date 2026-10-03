// Ablation driver for v4 (tools/ablate_v4/build.sh): v4 vs variants that each
// remove one cost, interleaved in one process, hardware counters per op.
#include "ablate.hpp"

#include <lob/book_config.hpp>
#include <lob/harness/perf_counters.hpp>
#include <lob/harness/tsc.hpp>
#include <lob/replay.hpp>
#include <lob/workload/format.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <print>
#include <string>
#include <vector>

using namespace lob;

struct NullSink {
    template <class E> void on(const E&) noexcept {}
};

template <class B, class S>
void run(std::span<const Op> ops, const BookConfig& cfg, harness::PerfGroup& g, std::vector<double>& cyc,
         std::vector<double>& ins) {
    S sink;
    auto book = make_book<B>(sink, cfg);
    g.start();
    replay(*book, ops);
    g.stop();
    harness::do_not_optimize(book->order_count());
    auto r = g.read();
    cyc.push_back(double(r.values[0]) / double(ops.size()));
    ins.push_back(double(r.values[1]) / double(ops.size()));
}

int main(int argc, char** argv) {
    auto w = workload::load(argv[1]);
    if (!w) { std::println("{}", w.error()); return 1; }
    const BookConfig cfg{.tick = infer_tick(w->ops)};
    const int rounds = argc > 2 ? std::stoi(argv[2]) : 9;
    harness::PerfGroup g(harness::default_counters());

    using Run = std::function<void(std::vector<double>&, std::vector<double>&)>;
    std::vector<std::pair<std::string, Run>> variants;
    auto add = [&]<class B, class S>(std::string name) {
        variants.emplace_back(name, [&, name](auto& c, auto& i) { run<B, S>(w->ops, cfg, g, c, i); });
    };
    add.template operator()<abl::BookT<ChecksumSink>, ChecksumSink>("v4 (baseline)");
    add.template operator()<abl::BookT<ChecksumSink, 16, false>, ChecksumSink>("A: no dup check (contains)");
    if (cfg.tick == 100)
        add.template operator()<abl::BookT<ChecksumSink, 16, true, 100>, ChecksumSink>("B: compile-time tick (no div)");
    else
        add.template operator()<abl::BookT<ChecksumSink, 16, true, 1>, ChecksumSink>("B: compile-time tick (no div)");
    add.template operator()<abl::BookT<NullSink>, NullSink>("C: null sink");

    std::map<std::string, std::pair<std::vector<double>, std::vector<double>>> res;
    for (int r = 0; r <= rounds; ++r)
        for (std::size_t k = 0; k < variants.size(); ++k) {
            auto& [name, fn] = variants[(k + r) % variants.size()];
            std::vector<double> c, i;
            fn(c, i);
            if (r > 0) { res[name].first.push_back(c[0]); res[name].second.push_back(i[0]); }
        }
    auto med = [](std::vector<double> v) { std::ranges::sort(v); return v[v.size() / 2]; };
    auto spread = [&](std::vector<double> v) { std::ranges::sort(v); return (v.back() - v.front()) / med(v); };
    const double base_c = med(res["v4 (baseline)"].first), base_i = med(res["v4 (baseline)"].second);
    std::println("{} tick={} rounds={}", argv[1], cfg.tick, rounds);
    for (auto& [name, fn] : variants) {
        auto& [c, i] = res[name];
        std::println("  {:<32} cycles/op {:7.2f} ({:+5.1f}%, spread {:4.1f}%)  instr/op {:7.2f} ({:+5.1f}%)", name, med(c),
                     100 * (med(c) - base_c) / base_c, 100 * spread(c), med(i), 100 * (med(i) - base_i) / base_i);
    }
}
