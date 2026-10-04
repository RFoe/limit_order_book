// Order-index prototypes, replayed in isolation (tools/index_proto/build.sh).
//
// The trace is the index work v12 does on a workload without trades (ITCH):
//   push   -> insert-if-absent(id) and store the order's node
//   cancel -> find(id) + erase, reading the node
//   modify -> find(id) + update (v12 keeps the slot on reprice as well)
// Candidates run interleaved round by round (same noise), hardware counters
// cover the replay loop only; medians over rounds are printed. Caveat: the
// rest of the book (nodes, levels, bitmaps) is absent, so the index has the
// caches to itself; absolute numbers are optimistic, relative ones are the point.
#include <lob/harness/perf_counters.hpp>
#include <lob/harness/tsc.hpp>
#include <lob/workload/format.hpp>

#include <boost/unordered/unordered_flat_map.hpp>

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <print>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using namespace lob;

enum class Kind : std::uint8_t { Insert, Erase, Update };
struct IxOp {
  Kind kind;
  std::uint32_t node;
  OrderId id;
};

auto trace_from(std::span<const Op> ops) -> std::vector<IxOp> {
  std::unordered_map<OrderId, bool> live;
  std::vector<IxOp> t;
  std::uint32_t node = 0;
  for (const Op& op : ops) {
    switch (op.type) {
      case OpType::Push:
        if (live.emplace(op.id, true).second) t.push_back({Kind::Insert, node++, op.id});
        break;
      case OpType::Cancel:
        if (live.erase(op.id)) t.push_back({Kind::Erase, 0, op.id});
        break;
      case OpType::Modify:
        if (live.contains(op.id)) t.push_back({Kind::Update, node++, op.id});
        break;
    }
  }
  return t;
}

// ---- candidates: insert(id, node) -> bool, erase(id) -> node, update(id, node) ----

struct Loc16 {  // v12's index value
  Price price;
  std::uint32_t node;
  Side side;
};

struct BoostWide {  // B0: v12 as is
  static constexpr const char* name = "B0 boost 24B (v12)";
  boost::unordered_flat_map<OrderId, Loc16> m;
  bool insert(OrderId id, std::uint32_t node) {
    auto [it, ok] = m.try_emplace(id);
    if (ok) it->second = Loc16{1, node, Side::Buy};
    return ok;
  }
  std::uint32_t erase(OrderId id) {
    auto it = m.find(id);
    const std::uint32_t n = it->second.node;
    m.erase(it);
    return n;
  }
  void update(OrderId id, std::uint32_t node) { m.find(id)->second.node = node; }
};

struct BoostNode {  // B8: same table, value = node index only
  static constexpr const char* name = "B8 boost 16B (u64->u32)";
  boost::unordered_flat_map<OrderId, std::uint32_t> m;
  bool insert(OrderId id, std::uint32_t node) { return m.try_emplace(id, node).second; }
  std::uint32_t erase(OrderId id) {
    auto it = m.find(id);
    const std::uint32_t n = it->second;
    m.erase(it);
    return n;
  }
  void update(OrderId id, std::uint32_t node) { m.find(id)->second = node; }
};

// Linear probing over cache-line-aligned slots, backward-shift deletion (no
// tombstones, so no drift and no rehash on churn), grows at max load 1/2.
// Slot16: {u64 key, u32 node} -> 4 slots per 64-byte line.
// Slot8 : {u32 key = id - base, u32 node} -> 8 per line (ids within 2^32 of base).
// Home: Fibonacci hashing, or (Loc) the id's high bits directly so that
// consecutive ids land in consecutive slots.
template <class Key, bool Locality>
class Linear {
  struct Slot {
    Key key;
    std::uint32_t node;
  };
  static constexpr Key kEmpty = ~Key{0};

 public:
  static constexpr const char* name = sizeof(Key) == 8 ? (Locality ? "L16-loc own 16B, id>>3" : "L16 own 16B")
                                                       : (Locality ? "L8-loc own 8B, id>>3" : "L8 own 8B");
  explicit Linear(OrderId base = 0) : base_(base) { alloc(1024); }

  bool insert(OrderId id, std::uint32_t node) {
    if (size_ + 1 > cap_ / 2) grow();
    const Key k = key(id);
    for (std::size_t i = home(k);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (s.key == k) return false;
      if (s.key == kEmpty) {
        s = Slot{k, node};
        ++size_;
        return true;
      }
    }
  }
  std::uint32_t erase(OrderId id) {
    const Key k = key(id);
    std::size_t i = home(k);
    while (slots_[i].key != k) i = (i + 1) & mask_;
    const std::uint32_t node = slots_[i].node;
    // backward shift: pull later members of the cluster into the hole when
    // their home position allows it
    for (std::size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
      const Key kj = slots_[j].key;
      if (kj == kEmpty) break;
      const std::size_t h = home(kj);
      // move j -> i unless h lies cyclically in (i, j]
      if (((j - h) & mask_) >= ((j - i) & mask_)) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i].key = kEmpty;
    --size_;
    return node;
  }
  void update(OrderId id, std::uint32_t node) {
    const Key k = key(id);
    std::size_t i = home(k);
    while (slots_[i].key != k) i = (i + 1) & mask_;
    slots_[i].node = node;
  }

 private:
  Key key(OrderId id) const { return static_cast<Key>(id - base_); }
  std::size_t home(Key k) const {
    if constexpr (Locality)
      return (static_cast<std::uint64_t>(k) >> 3) & mask_;
    else
      return static_cast<std::size_t>((static_cast<std::uint64_t>(k) * 0x9E3779B97F4A7C15ull) >> shift_);
  }
  void alloc(std::size_t cap) {
    cap_ = cap;
    mask_ = cap - 1;
    shift_ = 64 - static_cast<unsigned>(std::countr_zero(cap));
    mem_.reset(static_cast<Slot*>(::operator new(cap * sizeof(Slot), std::align_val_t{64})));
    slots_ = mem_.get();
    for (std::size_t i = 0; i < cap; ++i) slots_[i].key = kEmpty;
  }
  void grow() {
    auto old = std::move(mem_);
    const std::size_t old_cap = cap_;
    alloc(cap_ * 2);
    size_ = 0;
    for (std::size_t i = 0; i < old_cap; ++i)
      if (old.get()[i].key != kEmpty) {
        const Key k = old.get()[i].key;
        std::size_t j = home(k);
        while (slots_[j].key != kEmpty) j = (j + 1) & mask_;
        slots_[j] = old.get()[i];
        ++size_;
      }
  }
  struct Free {
    void operator()(Slot* p) const { ::operator delete(p, std::align_val_t{64}); }
  };
  std::unique_ptr<Slot, Free> mem_;
  Slot* slots_ = nullptr;
  std::size_t cap_ = 0, mask_ = 0, size_ = 0;
  unsigned shift_ = 0;
  OrderId base_;
};

// One 64-byte bucket per cache line: u32 key[8] + u32 node[8], keys = id - base.
// Lane 7 is not a slot: key[7] counts the entries that passed this bucket
// because it was full (exact: erase decrements along the entry's path), so a
// lookup stops at the first bucket that has no match and a zero counter. All
// 8 keys are compared with one AVX2 instruction (lane 7 masked off). One line
// per operation in the common case, no tombstones, no drift. Max load 1/2.
class Bucketed {
  struct alignas(64) Bucket {
    std::uint32_t key[8];
    std::uint32_t node[8];
  };
  static constexpr std::uint32_t kEmpty = ~0u;
  static constexpr unsigned kSlots = 7, kLanes = 0x7F;

 public:
  static constexpr const char* name = "S7 own SIMD 7x8B/line";
  explicit Bucketed(OrderId base) : base_(base) { alloc(256); }

  bool insert(OrderId id, std::uint32_t node) {
    if (size_ + 1 > nb_ * kSlots / 2) grow();
    const std::uint32_t k = key(id);
    const std::size_t b = home(k);
    for (std::size_t j = b;; j = (j + 1) & mask_) {  // duplicate?
      if (match(j, k)) return false;
      if (bk_[j].key[7] == 0) break;
    }
    place(b, k, node);
    ++size_;
    return true;
  }
  std::uint32_t erase(OrderId id) {
    const std::uint32_t k = key(id);
    const std::size_t b = home(k);
    for (std::size_t j = b;; j = (j + 1) & mask_)
      if (const unsigned m = match(j, k)) {
        const unsigned slot = static_cast<unsigned>(std::countr_zero(m));
        const std::uint32_t node = bk_[j].node[slot];
        bk_[j].key[slot] = kEmpty;
        for (std::size_t i = b; i != j; i = (i + 1) & mask_) --bk_[i].key[7];
        --size_;
        return node;
      }
  }
  void update(OrderId id, std::uint32_t node) {
    const std::uint32_t k = key(id);
    for (std::size_t j = home(k);; j = (j + 1) & mask_)
      if (const unsigned m = match(j, k)) {
        bk_[j].node[std::countr_zero(m)] = node;
        return;
      }
  }

 private:
  std::uint32_t key(OrderId id) const { return static_cast<std::uint32_t>(id - base_); }
  std::size_t home(std::uint32_t k) const {
    return static_cast<std::size_t>((std::uint64_t{k} * 0x9E3779B97F4A7C15ull) >> shift_);
  }
  unsigned lanes_equal(std::size_t j, std::uint32_t v) const {
    const __m256i keys = _mm256_load_si256(reinterpret_cast<const __m256i*>(bk_[j].key));
    const __m256i eq = _mm256_cmpeq_epi32(keys, _mm256_set1_epi32(static_cast<int>(v)));
    return static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(eq))) & kLanes;
  }
  unsigned match(std::size_t j, std::uint32_t k) const { return lanes_equal(j, k); }
  void place(std::size_t b, std::uint32_t k, std::uint32_t node) {
    for (std::size_t j = b;; j = (j + 1) & mask_) {
      if (const unsigned e = lanes_equal(j, kEmpty)) {
        const unsigned slot = static_cast<unsigned>(std::countr_zero(e));
        bk_[j].key[slot] = k;
        bk_[j].node[slot] = node;
        return;
      }
      ++bk_[j].key[7];  // passing a full bucket
    }
  }
  void alloc(std::size_t nb) {
    nb_ = nb;
    mask_ = nb - 1;
    shift_ = 64 - static_cast<unsigned>(std::countr_zero(nb));
    mem_.reset(static_cast<Bucket*>(::operator new(nb * sizeof(Bucket), std::align_val_t{64})));
    bk_ = mem_.get();
    for (std::size_t i = 0; i < nb; ++i) {
      for (auto& x : bk_[i].key) x = kEmpty;
      bk_[i].key[7] = 0;
    }
  }
  void grow() {
    auto old = std::move(mem_);
    const std::size_t old_nb = nb_;
    alloc(nb_ * 2);
    for (std::size_t i = 0; i < old_nb; ++i)
      for (unsigned s = 0; s < kSlots; ++s)
        if (old.get()[i].key[s] != kEmpty) place(home(old.get()[i].key[s]), old.get()[i].key[s], old.get()[i].node[s]);
  }
  struct Free {
    void operator()(Bucket* p) const { ::operator delete(p, std::align_val_t{64}); }
  };
  std::unique_ptr<Bucket, Free> mem_;
  Bucket* bk_ = nullptr;
  std::size_t nb_ = 0, mask_ = 0, size_ = 0;
  unsigned shift_ = 0;
  OrderId base_;
};

struct Result {
  double cyc, ins, brm, l1;
  std::uint64_t check;  // sum of erased nodes + inserts: must agree across candidates
};

template <class T>
auto run(const std::vector<IxOp>& t, OrderId base, harness::PerfGroup& g) -> Result {
  T m = [&] {
    if constexpr (std::is_constructible_v<T, OrderId>)
      return T(base);
    else
      return T{};
  }();
  std::uint64_t sink = 0;
  g.start();
  for (const IxOp& o : t) {
    switch (o.kind) {
      case Kind::Insert: sink += m.insert(o.id, o.node); break;
      case Kind::Erase: sink = sink * 31 + m.erase(o.id); break;
      case Kind::Update: m.update(o.id, o.node); break;
    }
  }
  g.stop();
  harness::do_not_optimize(sink);
  const auto r = g.read();
  const auto n = double(t.size());
  return {double(r.values[0]) / n, double(r.values[1]) / n, double(r.values[3]) / n, double(r.values[4]) / n, sink};
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::println("usage: proto WORKLOAD.ops [rounds]");
    return 2;
  }
  const int rounds = argc > 2 ? std::stoi(argv[2]) : 7;
  auto w = workload::load(argv[1]);
  if (!w) {
    std::println("{}", w.error());
    return 1;
  }
  const auto t = trace_from(w->ops);
  OrderId base = ~OrderId{0}, top = 0;
  for (const IxOp& o : t) base = std::min(base, o.id), top = std::max(top, o.id);
  const bool fits32 = top - base < 0xFFFFFFFFull;

  using Runner = Result (*)(const std::vector<IxOp>&, OrderId, harness::PerfGroup&);
  struct Cand {
    const char* name;
    Runner run;
    std::vector<Result> rs;
  };
  std::vector<Cand> cands = {
      {BoostWide::name, run<BoostWide>, {}},
      {BoostNode::name, run<BoostNode>, {}},
      {Linear<std::uint64_t, false>::name, run<Linear<std::uint64_t, false>>, {}},
  };
  if (fits32) {
    cands.push_back({Linear<std::uint32_t, false>::name, run<Linear<std::uint32_t, false>>, {}});
    cands.push_back({Bucketed::name, run<Bucketed>, {}});
  }
  harness::PerfGroup g(harness::default_counters());
  if (!g.ok()) {
    std::println("perf: {}", g.error());
    return 1;
  }
  for (int r = 0; r <= rounds; ++r)  // round 0 is warm-up; rotate the order each round
    for (std::size_t k = 0; k < cands.size(); ++k) {
      Cand& c = cands[(k + static_cast<std::size_t>(r)) % cands.size()];
      const Result x = c.run(t, base, g);
      if (r > 0) c.rs.push_back(x);
    }
  std::println("{}: {} index ops (insert/erase/update), id span {}", argv[1], t.size(), top - base);
  for (const Cand& c : cands)
    if (c.rs.front().check != cands.front().rs.front().check) {
      std::println("CHECK MISMATCH: {} vs {}", c.name, cands.front().name);
      return 1;
    }
  for (const Cand& c : cands) {
    auto med = [&](auto f) {
      std::vector<double> v;
      for (const auto& x : c.rs) v.push_back(f(x));
      std::ranges::sort(v);
      return v[v.size() / 2];
    };
    const double cyc = med([](auto& x) { return x.cyc; }), ins = med([](auto& x) { return x.ins; });
    std::println("  {:<26} cycles/op {:6.2f}  instr/op {:6.2f}  IPC {:4.2f}  br-miss/op {:5.3f}  L1d-miss/op {:5.3f}",
                 c.name, cyc, ins, ins / cyc, med([](auto& x) { return x.brm; }), med([](auto& x) { return x.l1; }));
  }
}
