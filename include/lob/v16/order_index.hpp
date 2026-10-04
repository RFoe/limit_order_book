#pragma once
// Order index for v16: v15's table with an insert that does not read the bucket.
//   - occupancy bytes, one per bucket (bit s: slot s in use; 8 KB for AAPL's
//     8192 buckets, so L1-resident): a free slot is chosen from them instead of
//     comparing the bucket's keys with kEmpty
//   - next_new_: every id >= it has never been inserted, so it cannot be a
//     duplicate (ITCH push ids are 99.4% strictly increasing)
// A push whose id is >= next_new_ therefore only writes the bucket line (key,
// later the node): a store miss goes to the store buffer instead of a load
// miss on the op's critical path (v15's push p99 on AAPL +22% vs v12).
// Lookups (cancel, modify, the duplicate check of an older id) are unchanged.
//
// v15's description follows.
//
// Order index for v15: OrderId -> u32 order-node index, one cache line per bucket.
//
//   bucket (64 B, aligned): u32 key[8] + u32 node[8]; key = the id
//   lanes 0..6 are slots (key kEmpty = free); lane 7 is not a slot: key[7]
//   counts the entries that were placed past this bucket because it was full
//
// Lookup of key k: compare the bucket's 8 keys with one AVX2 instruction
// (lane 7 masked off); stop at the first bucket with no match and a zero
// counter. Insert: the first bucket with a free slot, incrementing the counter
// of every full bucket passed; erase decrements them again along the same path,
// so counters are exact (no tombstones, no drift, no rehash on churn). Max load
// 1/2 (3.5 entries per 7-slot bucket on average), doubling when exceeded.
//
// The common operation touches one cache line: key and value share it, and an
// insert writes the line the lookup just read. Keys are the ids themselves, 32
// bits (ITCH order reference numbers of a day and the generator's ids are far
// below 2^32); an id >= 2^32 - 1 goes to a (cold) boost::unordered_flat_map
// fallback. A Handle is two registers wide ({value pointer, bucket, home}): the
// first version carried slot / fallback flag / id as well and returned it in a
// std::pair, which the compiler kept in memory (about 7 extra stores per op).
// Prototype and measurements: tools/index_proto, docs/experiments.md.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include <boost/unordered/unordered_flat_map.hpp>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace lob::v16 {

class OrderIndex {
 public:
  using Id = std::uint64_t;

  // where an entry lives; stays valid until the entry itself is erased or the
  // index grows (erasing other entries never moves anything)
  struct Handle {
    std::uint32_t* value = nullptr;  // the stored node index
    std::uint32_t bucket = 0;        // kFallback: the entry is in the fallback map
    std::uint32_t home = 0;
    explicit operator bool() const noexcept { return value != nullptr; }
  };
  static constexpr std::uint32_t kFallback = ~std::uint32_t{0};

  OrderIndex() { alloc(kInitialBuckets); }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_ + fallback_.size(); }

  // the hot operations are always_inline: they sit on the book's pinned hot
  // path (v12), and left to the heuristics try_insert / erase stayed calls
  [[gnu::always_inline]] [[nodiscard]] auto find(Id id) -> Handle {
    if (id >= kEmpty) [[unlikely]]
      return find_fallback(id);
    const auto k = static_cast<std::uint32_t>(id);
    const std::size_t b = home(k);
    for (std::size_t j = b;; j = (j + 1) & mask_) {
      if (const unsigned m = lanes_equal(j, k)) return handle(b, j, static_cast<unsigned>(std::countr_zero(m)));
      if (bk_[j].key[kCounter] == 0) return {};
    }
  }
  [[nodiscard]] auto find(Id id) const -> const std::uint32_t* {
    return const_cast<OrderIndex*>(this)->find(id).value;
  }

  // the entry for id, inserting one (value unset) if absent
  [[gnu::always_inline]] auto try_insert(Id id, bool& inserted) -> Handle {
    if (id >= next_new_ && id < kEmpty) [[likely]] {  // never inserted: no duplicate check, no load
      next_new_ = id + 1;
      if (size_ >= limit_) [[unlikely]]
        grow();
      const auto k = static_cast<std::uint32_t>(id);
      const std::size_t b = home(k);
      const auto [j, slot] = place(b, k);
      ++size_;
      inserted = true;
      return handle(b, j, slot);
    }
    if (id >= kEmpty) [[unlikely]]
      return insert_fallback(id, inserted);
    if (size_ >= limit_) [[unlikely]]
      grow();
    const auto k = static_cast<std::uint32_t>(id);
    const std::size_t b = home(k);
    for (std::size_t j = b;; j = (j + 1) & mask_) {  // already present?
      if (const unsigned m = lanes_equal(j, k)) {
        inserted = false;
        return handle(b, j, static_cast<unsigned>(std::countr_zero(m)));
      }
      if (bk_[j].key[kCounter] == 0) break;
    }
    const auto [j, slot] = place(b, k);
    ++size_;
    inserted = true;
    return handle(b, j, slot);
  }

  // id: the entry's key (only the fallback map needs it)
  [[gnu::always_inline]] void erase(const Handle& h, Id id) {
    if (h.bucket == kFallback) [[unlikely]] {
      fallback_.erase(id);
      return;
    }
    const auto slot = static_cast<unsigned>(h.value - bk_[h.bucket].node);
    bk_[h.bucket].key[slot] = kEmpty;
    occ_[h.bucket] &= static_cast<std::uint8_t>(~(1u << slot));
    for (std::size_t i = h.home; i != h.bucket; i = (i + 1) & mask_) --bk_[i].key[kCounter];
    --size_;
  }

  // memory touched by the table (for book memory_info)
  [[nodiscard]] auto bytes() const noexcept -> std::size_t { return nb_ * (sizeof(Bucket) + 1); }

 private:
  struct alignas(64) Bucket {
    std::uint32_t key[8];
    std::uint32_t node[8];
  };
  static_assert(sizeof(Bucket) == 64);
  static constexpr std::uint32_t kEmpty = ~std::uint32_t{0};
  static constexpr unsigned kSlots = 7, kCounter = 7, kSlotLanes = 0x7F;
  static constexpr std::size_t kInitialBuckets = 256;

  [[nodiscard]] auto home(std::uint32_t k) const -> std::size_t {
    return static_cast<std::size_t>((std::uint64_t{k} * 0x9E3779B97F4A7C15ull) >> shift_);  // Fibonacci hashing
  }
  [[nodiscard]] auto handle(std::size_t b, std::size_t j, unsigned slot) -> Handle {
    return {&bk_[j].node[slot], static_cast<std::uint32_t>(j), static_cast<std::uint32_t>(b)};
  }
  // bit s set when key[s] == v, for the 7 slot lanes
  [[nodiscard]] auto lanes_equal(std::size_t j, std::uint32_t v) const -> unsigned {
#if defined(__AVX2__)
    const __m256i keys = _mm256_load_si256(reinterpret_cast<const __m256i*>(bk_[j].key));
    const __m256i eq = _mm256_cmpeq_epi32(keys, _mm256_set1_epi32(static_cast<int>(v)));
    return static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(eq))) & kSlotLanes;
#else
    unsigned m = 0;
    for (unsigned s = 0; s < kSlots; ++s) m |= unsigned{bk_[j].key[s] == v} << s;
    return m;
#endif
  }
  struct Placed {
    std::size_t bucket;
    unsigned slot;
  };
  // free slot from the occupancy byte: the bucket line is only written
  [[gnu::always_inline]] auto place(std::size_t b, std::uint32_t k) -> Placed {
    for (std::size_t j = b;; j = (j + 1) & mask_) {
      const unsigned occupied = occ_[j];
      if (const unsigned free = ~occupied & kSlotLanes) {
        const auto slot = static_cast<unsigned>(std::countr_zero(free));
        occ_[j] = static_cast<std::uint8_t>(occupied | (1u << slot));
        bk_[j].key[slot] = k;
        return {j, slot};
      }
      ++bk_[j].key[kCounter];  // passing a full bucket (read-modify-write, rare)
    }
  }
  void alloc(std::size_t nb) {
    nb_ = nb;
    limit_ = nb * kSlots / 2;  // max load 1/2
    mask_ = nb - 1;
    shift_ = 64 - static_cast<unsigned>(std::countr_zero(nb));
    mem_.reset(static_cast<Bucket*>(::operator new(nb * sizeof(Bucket), std::align_val_t{64})));
    bk_ = mem_.get();
    occ_ = std::make_unique<std::uint8_t[]>(nb);  // zeroed: all slots free
    for (std::size_t i = 0; i < nb; ++i) {
      for (auto& x : bk_[i].key) x = kEmpty;
      bk_[i].key[kCounter] = 0;
    }
  }
  [[gnu::noinline]] void grow() {
    auto old = std::move(mem_);
    const std::size_t old_nb = nb_;
    alloc(nb_ * 2);
    for (std::size_t i = 0; i < old_nb; ++i)
      for (unsigned s = 0; s < kSlots; ++s)
        if (const std::uint32_t k = old.get()[i].key[s]; k != kEmpty) {
          const auto [j, slot] = place(home(k), k);
          bk_[j].node[slot] = old.get()[i].node[s];
        }
  }
  [[gnu::noinline]] auto find_fallback(Id id) -> Handle {
    const auto it = fallback_.find(id);
    if (it == fallback_.end()) return {};
    return {&it->second, kFallback, 0};
  }
  [[gnu::noinline]] auto insert_fallback(Id id, bool& inserted) -> Handle {
    const auto r = fallback_.try_emplace(id, 0u);
    inserted = r.second;
    return {&r.first->second, kFallback, 0};
  }

  struct Free {
    void operator()(Bucket* p) const { ::operator delete(p, std::align_val_t{64}); }
  };
  std::unique_ptr<Bucket, Free> mem_;
  Bucket* bk_ = nullptr;
  std::unique_ptr<std::uint8_t[]> occ_;  // per bucket: bit s = slot s in use
  std::size_t nb_ = 0, limit_ = 0, mask_ = 0, size_ = 0;
  Id next_new_ = 0;  // ids >= next_new_ were never inserted
  unsigned shift_ = 0;
  boost::unordered_flat_map<Id, std::uint32_t> fallback_;  // ids outside the 32-bit window
};

}  // namespace lob::v16
