// Index-only replay (tools/index_trace/build.sh): the exact sequence of
// boost::unordered_flat_map operations v4 performs on a no-trade workload
// (ITCH: push = contains + emplace, cancel = find + erase, modify = find +
// update or find + erase + emplace), replayed against the same map type with
// the same 16-byte Loc, plus synthetic traces with the same operation mix and
// a fixed number of live keys (FIFO cancels) to separate instruction cost from
// memory cost. Counts only the replay loop (PerfGroup + PerfControl, so perf
// stat -D -1 --control can add named events).
#ifdef LOB_IX_STATS // probe statistics build (slower; not for timing)
#define BOOST_UNORDERED_ENABLE_STATS
#endif
#include <lob/harness/perf_counters.hpp>
#include <lob/harness/tsc.hpp>
#include <lob/workload/format.hpp>

#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

using namespace lob;

struct Loc { // v4's packed Loc
    Price         price;
    std::uint32_t node;
    Side          side;
};
static_assert(sizeof(Loc) == 16);

enum class Kind : std::uint8_t { Contains, Emplace, FindErase, FindUpdate };
struct IxOp {
    Kind          kind;
    std::uint32_t node;
    OrderId       id;
    Price         price;
};

// v4's index operations for a workload without trades (every push rests)
auto trace_from(std::span<const Op> ops) -> std::vector<IxOp> {
    struct St {
        Price price;
        Qty   qty;
    };
    std::unordered_map<OrderId, St> live;
    std::vector<IxOp>               t;
    std::uint32_t                   node = 0;
    for (const Op &op : ops) {
        switch (op.type) {
        case OpType::Push:
            t.push_back({Kind::Contains, 0, op.id, 0});
            t.push_back({Kind::Emplace, node++, op.id, op.price});
            live[op.id] = {op.price, op.qty};
            break;
        case OpType::Cancel:
            if (live.erase(op.id)) t.push_back({Kind::FindErase, 0, op.id, 0});
            break;
        case OpType::Modify: {
            auto it = live.find(op.id);
            if (it == live.end()) break;
            if (op.price == it->second.price && op.qty <= it->second.qty) {
                t.push_back({Kind::FindUpdate, 0, op.id, 0});
                it->second.qty = op.qty;
            } else {
                t.push_back({Kind::FindErase, 0, op.id, 0});
                t.push_back({Kind::Emplace, node++, op.id, op.price});
                it->second = {op.price, op.qty};
            }
            break;
        }
        }
    }
    return t;
}

// same mix (push = contains + emplace, then a cancel), `live` keys alive at once
auto synthetic(std::size_t live, std::size_t n_push) -> std::vector<IxOp> {
    std::vector<IxOp> t;
    for (std::size_t i = 0; i < n_push; ++i) {
        const OrderId id = i + 1;
        t.push_back({Kind::Contains, 0, id, 0});
        t.push_back({Kind::Emplace, std::uint32_t(i), id, Price(id)});
        if (i >= live) t.push_back({Kind::FindErase, 0, id - live, 0});
    }
    return t;
}

struct Result {
    double cyc, ins, brm, l1, ratio;
};

auto run(const std::vector<IxOp> &t, harness::PerfGroup &g, harness::PerfControl &ctl) -> Result {
    boost::unordered_flat_map<OrderId, Loc> m;
    if (const char *r = std::getenv("LOB_IX_RESERVE")) m.reserve(std::stoul(r)); // pre-size (experiment)
    std::uint64_t                           sink = 0;
#ifdef LOB_IX_STATS
    std::size_t rehashes = 0;
#endif
    ctl.enable();
    g.start();
    for (const IxOp &o : t) {
        switch (o.kind) {
        case Kind::Contains: sink += m.contains(o.id); break;
        case Kind::Emplace:
#ifdef LOB_IX_STATS
        {
            const auto before = m.bucket_count();
            m.emplace(o.id, Loc{o.price, o.node, Side::Buy});
            rehashes += m.bucket_count() != before;
            break;
        }
#else
            m.emplace(o.id, Loc{o.price, o.node, Side::Buy});
            break;
#endif
        case Kind::FindErase: {
            auto it = m.find(o.id);
            sink += it->second.node;
            m.erase(it);
            break;
        }
        case Kind::FindUpdate: m.find(o.id)->second.node ^= 1; break;
        }
    }
    g.stop();
    ctl.disable();
    harness::do_not_optimize(sink);
#ifdef LOB_IX_STATS
    const auto st = m.get_stats();
    std::println("  stats: rehashes={} final buckets={} | insert n={} probe={:.3f} | hit n={} probe={:.3f} cmp={:.3f} "
                 "| miss n={} probe={:.3f} cmp={:.3f}",
                 rehashes, m.bucket_count(), st.insertion.count, st.insertion.probe_length.average,
                 st.successful_lookup.count, st.successful_lookup.probe_length.average,
                 st.successful_lookup.num_comparisons.average, st.unsuccessful_lookup.count,
                 st.unsuccessful_lookup.probe_length.average, st.unsuccessful_lookup.num_comparisons.average);
#endif
    auto       r = g.read();
    const auto n = double(t.size());
    return {double(r.values[0]) / n, double(r.values[1]) / n, double(r.values[3]) / n, double(r.values[4]) / n,
            r.running_ratio};
}

// same live-set size, but push/cancel interleave at random and cancels pick a
// random live key (irregular like ITCH, unlike the strictly periodic syn:)
auto random_mix(std::size_t live, std::size_t n_push, std::uint64_t seed) -> std::vector<IxOp> {
    std::vector<IxOp>    t;
    std::vector<OrderId> keys;
    std::uint64_t        x = seed;
    auto next = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    OrderId id = 0;
    for (std::size_t pushes = 0; pushes < n_push;) {
        if (keys.size() < live / 2 || (keys.size() < live && next() % 2 == 0)) {
            ++id;
            ++pushes;
            t.push_back({Kind::Contains, 0, id, 0});
            t.push_back({Kind::Emplace, std::uint32_t(pushes), id, Price(id)});
            keys.push_back(id);
        } else {
            const std::size_t k = next() % keys.size();
            t.push_back({Kind::FindErase, 0, keys[k], 0});
            keys[k] = keys.back();
            keys.pop_back();
        }
    }
    return t;
}

int main(int argc, char **argv) {
    // argv[1]: workload, "syn:<live>" (periodic) or "rnd:<live>" (random mix); argv[2]: rounds
    const std::string    what   = argv[1];
    const int            rounds = argc > 2 ? std::stoi(argv[2]) : 7;
    std::vector<IxOp>    t;
    if (what.starts_with("syn:")) {
        t = synthetic(std::stoul(what.substr(4)), 800'000);
    } else if (what.starts_with("rnd:")) {
        t = random_mix(std::stoul(what.substr(4)), 800'000, 42);
    } else {
        auto w = workload::load(what);
        if (!w) {
            std::println("{}", w.error());
            return 1;
        }
        t = trace_from(w->ops);
    }
    harness::PerfGroup   g(harness::default_counters());
    harness::PerfControl ctl;
    std::vector<Result>  rs;
    for (int r = 0; r <= rounds; ++r) {
        auto x = run(t, g, ctl);
        if (r > 0) rs.push_back(x);
    }
    auto med = [&](auto f) {
        std::vector<double> v;
        for (auto &x : rs) v.push_back(f(x));
        std::ranges::sort(v);
        return v[v.size() / 2];
    };
    std::println("{:<34} index ops {:>9}  cycles/op {:6.2f}  instr/op {:6.2f}  IPC {:4.2f}  br-miss/op {:5.3f}  "
                 "L1d-miss/op {:5.3f}",
                 what, t.size(), med([](auto &x) { return x.cyc; }), med([](auto &x) { return x.ins; }),
                 med([](auto &x) { return x.ins; }) / med([](auto &x) { return x.cyc; }),
                 med([](auto &x) { return x.brm; }), med([](auto &x) { return x.l1; }));
}
