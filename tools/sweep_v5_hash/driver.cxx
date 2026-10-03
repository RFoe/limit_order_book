// Hash sweep for v5 (tools/sweep_v5_hash/build.sh): v4 with its default index
// hash vs v4 with a locality-preserving hash for several block sizes K,
// interleaved in one process, hardware counters per op.
//
// LocalityHash<K>: ids in the same block (id >> K) get the same high bits, which
// boost::unordered_flat_map uses to pick the 15-slot group, so orders inserted
// close in time share groups (cache lines). The low byte, which boost uses as
// the in-group fingerprint, varies with id. Marked avalanching so boost does
// not re-mix it.
#include "sweep.hpp"

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
#include <type_traits>
#include <vector>

using namespace lob;

template <unsigned K> struct LocalityHash {
    using is_avalanching = std::true_type;
    auto operator()(OrderId id) const noexcept -> std::size_t {
        const auto          r = static_cast<unsigned __int128>(id >> K) * 0x9E3779B97F4A7C15ULL;
        const std::uint64_t m = static_cast<std::uint64_t>(r) ^ static_cast<std::uint64_t>(r >> 64);
        return (m & ~std::uint64_t{0xFF}) | ((m ^ id) & 0xFF);
    }
};

struct Stats {
    std::vector<double> cyc, ins, l1;
};

template <class B>
void run(std::span<const Op> ops, const BookConfig& cfg, harness::PerfGroup& g, Stats& s) {
    ChecksumSink sink;
    auto         book = make_book<B>(sink, cfg);
    g.start();
    replay(*book, ops);
    g.stop();
    harness::do_not_optimize(sink.value);
    auto       r = g.read();
    const auto n = double(ops.size());
    s.cyc.push_back(double(r.values[0]) / n);
    s.ins.push_back(double(r.values[1]) / n);
    s.l1.push_back(double(r.values[4]) / n);
}

int main(int argc, char **argv) {
    auto w = workload::load(argv[1]);
    if (!w) {
        std::println("{}", w.error());
        return 1;
    }
    const BookConfig   cfg{.tick = infer_tick(w->ops)};
    const int          rounds = argc > 2 ? std::stoi(argv[2]) : 7;
    harness::PerfGroup g(harness::default_counters());

    std::vector<std::pair<std::string, std::function<void(Stats &)>>> variants;
    auto add = [&]<class B>(std::string name) {
        variants.emplace_back(name, [&](Stats &s) { run<B>(w->ops, cfg, g, s); });
    };
    add.template operator()<swp::BookT<ChecksumSink>>("v4 (boost default hash)");
    add.template operator()<swp::BookT<ChecksumSink, 16, LocalityHash<0>>>("K=0  (custom random)");
    add.template operator()<swp::BookT<ChecksumSink, 16, LocalityHash<3>>>("K=3  (8 ids/block)");
    add.template operator()<swp::BookT<ChecksumSink, 16, LocalityHash<4>>>("K=4  (16 ids/block)");
    add.template operator()<swp::BookT<ChecksumSink, 16, LocalityHash<5>>>("K=5  (32 ids/block)");
    add.template operator()<swp::BookT<ChecksumSink, 16, LocalityHash<6>>>("K=6  (64 ids/block)");
    add.template operator()<swp::BookT<ChecksumSink, 16, LocalityHash<8>>>("K=8  (256 ids/block)");
    add.template operator()<swp::BookT<ChecksumSink, 16, LocalityHash<10>>>("K=10 (1024 ids/block)");
    // same, with the table pre-sized to 2^17 slots (load ~0.2 at AAPL's 27k peak):
    // gives each id block room in its group instead of overflowing it
    constexpr std::size_t kRes = std::size_t{1} << 17;
    add.template operator()<swp::BookT<ChecksumSink, 16, boost::hash<OrderId>, kRes>>("reserve: default hash");
    add.template operator()<swp::BookT<ChecksumSink, 16, LocalityHash<3>, kRes>>("reserve: K=3");
    add.template operator()<swp::BookT<ChecksumSink, 16, LocalityHash<4>, kRes>>("reserve: K=4");
    add.template operator()<swp::BookT<ChecksumSink, 16, LocalityHash<6>, kRes>>("reserve: K=6");

    std::map<std::string, Stats> res;
    for (int r = 0; r <= rounds; ++r)
        for (std::size_t k = 0; k < variants.size(); ++k) {
            auto &[name, fn] = variants[(k + r) % variants.size()];
            Stats s;
            fn(s);
            if (r > 0) {
                res[name].cyc.push_back(s.cyc[0]);
                res[name].ins.push_back(s.ins[0]);
                res[name].l1.push_back(s.l1[0]);
            }
        }
    auto med = [](std::vector<double> v) {
        std::ranges::sort(v);
        return v[v.size() / 2];
    };
    auto spread = [&](std::vector<double> v) {
        std::ranges::sort(v);
        return (v.back() - v.front()) / med(v);
    };
    const auto &b = res["v4 (boost default hash)"];
    std::println("{} tick={} rounds={}", argv[1], cfg.tick, rounds);
    for (auto &[name, fn] : variants) {
        auto &s = res[name];
        std::println("  {:<26} cycles/op {:7.2f} ({:+5.1f}%, spread {:4.1f}%)  instr/op {:7.2f} ({:+5.1f}%)  "
                     "L1d-miss/op {:5.2f} ({:+5.1f}%)",
                     name, med(s.cyc), 100 * (med(s.cyc) - med(b.cyc)) / med(b.cyc), 100 * spread(s.cyc), med(s.ins),
                     100 * (med(s.ins) - med(b.ins)) / med(b.ins), med(s.l1),
                     100 * (med(s.l1) - med(b.l1)) / med(b.l1));
    }
}
