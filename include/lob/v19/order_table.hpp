#pragma once
// Order table for v19: the order index and the order-node pool merged into one
// open-addressing table keyed by order id, so the node of an order lives in the
// cache line its lookup lands on.
//
//   bucket (64 B, aligned): u32 key[4] + OrderNode node[3]
//     key[0..2] the ids of slots 0..2 (kEmpty when free), key[3] counts the
//     entries placed past this bucket because it was full (exact: erase
//     decrements along the entry's path)
//   occ_[bucket] (1 byte, outside the bucket): bit s = slot s in use
//
// Lookup (cancel, modify, duplicate check of an older id): compare key[0..3]
// with one 128-bit compare, slot = first match in lanes 0..2 (a real id never
// equals kEmpty), stop at the first bucket with no match and a zero counter -
// one cache line in the common case, and that line holds the node. Insert of an
// id above every id inserted so far (cannot be a duplicate; ITCH: 99.4%) picks
// a free slot from occ_ and only writes the bucket line (v16's idea). Erase
// clears the key and the occupancy bit; nothing else moves.
//
// A node is referenced by `ref` = bucket * 4 + slot. References change only
// when the table grows (rare: warm-up); grow() fixes the nodes' own prev / next
// and leaves an old -> new map (remap()) for the owner to fix the references it
// holds (level heads / tails), then clear_remap().
//
// Ids >= 2^32 - 1 live in a small separate pool (refs with bit 31 set) behind a
// boost map. node() selects the base (table or pool) with that bit and indexes
// it, without a branch: the table's nodes are addressable as one array because
// node[s] of bucket b sits at 16 + 16 * s bytes into it, i.e. at
// (bk_ + 16 bytes) + 16 * ref (slot 3 never exists). A branch there (first
// version) turned unlink's / push_back's branch-free link selection (v8) back
// into mispredicting branches (+0.5 mispredicts/op on AAPL).
//
// Max load 1/3 (one entry per 3-slot bucket on average): at 1/2 (first
// version) AAPL spent most of the day near the limit and 12.5% of inserts
// landed outside their home bucket (second line, counter updates, branches).

#include <lob/types.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#if defined(__SSE2__)
#include <immintrin.h>
#endif

namespace lob::v19 {

struct OrderNode {       // hot part of an order (v18 layout)
  Qty qty;
  std::uint32_t next;   // level list, towards the tail
  std::uint32_t prev;
  std::uint32_t where;  // grid slot (or off-grid marker) | side << 31, owned by the book
};
static_assert(sizeof(OrderNode) == 16);

class OrderTable {
 public:
  using Id = std::uint64_t;
  static constexpr std::uint32_t kNil = ~std::uint32_t{0};
  static constexpr std::uint32_t kBig = std::uint32_t{1} << 31;  // ref tag: big-id pool

  OrderTable() { alloc(kInitialBuckets); }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_ + big_index_.size(); }

  // the node of a live ref (ref != kNil); branch-free base selection
  [[gnu::always_inline]] auto node(std::uint32_t ref) -> OrderNode& {
    OrderNode* base = __builtin_unpredictable(ref & kBig) ? big_nodes_.data() : nodes_;
    return base[ref & ~kBig];
  }
  [[gnu::always_inline]] auto node(std::uint32_t ref) const -> const OrderNode& {
    return const_cast<OrderTable*>(this)->node(ref);
  }
  [[nodiscard]] auto id_of(std::uint32_t ref) const -> Id {
    if (ref & kBig) [[unlikely]]
      return big_ids_[ref & ~kBig];
    return bk_[ref >> 2].key[ref & 3];
  }

  // ref of id, kNil if absent
  [[gnu::always_inline]] [[nodiscard]] auto find(Id id) const -> std::uint32_t {
    if (id >= kEmpty) [[unlikely]]
      return find_big(id);
    const auto k = static_cast<std::uint32_t>(id);
    for (std::size_t j = home(k);; j = next(j)) {
      if (const unsigned m = match(j, k)) return ref(j, static_cast<unsigned>(std::countr_zero(m)));
      if (bk_[j].key[kCounter] == 0) return kNil;
    }
  }

  // ref of id, inserting a slot (node unset) if absent. May grow: then
  // grew() is true and remap() holds the old -> new references until clear_remap().
  [[gnu::always_inline]] auto try_insert(Id id, bool& inserted) -> std::uint32_t {
    if (id >= next_new_ && id < kEmpty) [[likely]] {  // never inserted: no duplicate check, no load
      next_new_ = id + 1;
      if (size_ >= limit_) [[unlikely]]
        grow();
      const auto k = static_cast<std::uint32_t>(id);
      inserted = true;
      ++size_;
      return place(home(k), k);
    }
    if (id >= kEmpty) [[unlikely]]
      return insert_big(id, inserted);
    if (size_ >= limit_) [[unlikely]]
      grow();
    const auto k = static_cast<std::uint32_t>(id);
    const std::size_t b = home(k);
    for (std::size_t j = b;; j = next(j)) {  // already present?
      if (const unsigned m = match(j, k)) {
        inserted = false;
        return ref(j, static_cast<unsigned>(std::countr_zero(m)));
      }
      if (bk_[j].key[kCounter] == 0) break;
    }
    inserted = true;
    ++size_;
    return place(b, k);
  }

  // frees a live ref; other refs are unaffected
  [[gnu::always_inline]] void erase(std::uint32_t r) {
    if (r & kBig) [[unlikely]]
      return erase_big(r);
    const std::size_t j = r >> 2;
    const unsigned s = r & 3;
    const std::uint32_t k = bk_[j].key[s];
    bk_[j].key[s] = kEmpty;
    occ_[j] = static_cast<std::uint8_t>(occ_[j] & ~(1u << s));
    for (std::size_t i = home(k); i != j; i = next(i)) --bk_[i].key[kCounter];
    --size_;
  }

  // ---- growth bookkeeping for the owner -------------------------------------------
  [[nodiscard]] auto grew() const noexcept -> bool { return !remap_.empty(); }
  [[nodiscard]] auto remap(std::uint32_t old_ref) const -> std::uint32_t {
    if (old_ref == kNil || (old_ref & kBig)) return old_ref;
    return remap_[old_ref];
  }
  void clear_remap() { remap_.clear(); }

  [[nodiscard]] auto bytes() const noexcept -> std::size_t { return nb_ * (sizeof(Bucket) + 1); }
  [[nodiscard]] auto buckets() const noexcept -> std::size_t { return nb_; }

 private:
  struct alignas(64) Bucket {
    std::uint32_t key[4];
    OrderNode node[3];
  };
  static_assert(sizeof(Bucket) == 64);
  static constexpr std::uint32_t kEmpty = ~std::uint32_t{0};
  static constexpr unsigned kSlots = 3, kCounter = 3, kSlotLanes = 0x7;
  static constexpr std::size_t kInitialBuckets = 256;

  // bucket of key k: 32-bit multiplicative hash, scaled to [0, nb) (any nb)
  [[nodiscard]] auto home(std::uint32_t k) const -> std::size_t {
    return static_cast<std::size_t>((std::uint64_t{k * 0x9E3779B9u} * nb_) >> 32);
  }
  [[nodiscard]] auto next(std::size_t j) const -> std::size_t { return j + 1 == nb_ ? 0 : j + 1; }
  [[nodiscard]] static auto ref(std::size_t j, unsigned s) -> std::uint32_t {
    return static_cast<std::uint32_t>(j << 2 | s);
  }
  // bit s set when key[s] == k, for the 3 slots
  [[nodiscard]] auto match(std::size_t j, std::uint32_t k) const -> unsigned {
#if defined(__SSE2__)
    const __m128i keys = _mm_load_si128(reinterpret_cast<const __m128i*>(bk_[j].key));
    const __m128i eq = _mm_cmpeq_epi32(keys, _mm_set1_epi32(static_cast<int>(k)));
    return static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(eq))) & kSlotLanes;
#else
    unsigned m = 0;
    for (unsigned s = 0; s < kSlots; ++s) m |= unsigned{bk_[j].key[s] == k} << s;
    return m;
#endif
  }
  // free slot from the occupancy byte: the bucket line is only written
  [[gnu::always_inline]] auto place(std::size_t b, std::uint32_t k) -> std::uint32_t {
    for (std::size_t j = b;; j = next(j)) {
      const unsigned occupied = occ_[j];
      if (const unsigned free = ~occupied & kSlotLanes) {
        const auto s = static_cast<unsigned>(std::countr_zero(free));
        occ_[j] = static_cast<std::uint8_t>(occupied | (1u << s));
        bk_[j].key[s] = k;
        return ref(j, s);
      }
      ++bk_[j].key[kCounter];  // passing a full bucket (read-modify-write, rare)
    }
  }
  void alloc(std::size_t nb) {
    nb_ = nb;
    limit_ = nb;  // max load 1/3: one entry per 3-slot bucket on average
    mem_.reset(static_cast<Bucket*>(::operator new(nb * sizeof(Bucket), std::align_val_t{64})));
    bk_ = mem_.get();
    // node[s] of bucket b at 16 + 16 * s bytes into it: nodes_[b * 4 + s]
    nodes_ = reinterpret_cast<OrderNode*>(reinterpret_cast<char*>(bk_) + offsetof(Bucket, node));
    for (std::size_t i = 0; i < nb; ++i) {
      for (auto& x : bk_[i].key) x = kEmpty;
      bk_[i].key[kCounter] = 0;
    }
    occ_ = std::make_unique<std::uint8_t[]>(nb);
  }
  [[gnu::noinline]] void grow() {
    auto old_mem = std::move(mem_);
    auto old_occ = std::move(occ_);
    const std::size_t old_nb = nb_;
    alloc(nb_ * 2);
    remap_.assign(old_nb * 4, kNil);
    for (std::size_t j = 0; j < old_nb; ++j)
      for (unsigned s = 0; s < kSlots; ++s)
        if (old_occ[j] & (1u << s)) {
          const std::uint32_t k = old_mem.get()[j].key[s];
          const std::uint32_t r = place(home(k), k);
          bk_[r >> 2].node[r & 3] = old_mem.get()[j].node[s];
          remap_[ref(j, s)] = r;
        }
    // the nodes' own links (main table and big pool)
    for (std::size_t j = 0; j < nb_; ++j)
      for (unsigned s = 0; s < kSlots; ++s)
        if (occ_[j] & (1u << s)) {
          OrderNode& n = bk_[j].node[s];
          n.next = remap(n.next);
          n.prev = remap(n.prev);
        }
    for (const auto& [id, i] : big_index_) {
      OrderNode& n = big_nodes_[i];
      n.next = remap(n.next);
      n.prev = remap(n.prev);
    }
  }

  // ---- ids >= 2^32 - 1 (cold) --------------------------------------------------------
  [[gnu::noinline]] auto find_big(Id id) const -> std::uint32_t {
    const auto it = big_index_.find(id);
    return it == big_index_.end() ? kNil : (kBig | it->second);
  }
  [[gnu::noinline]] auto insert_big(Id id, bool& inserted) -> std::uint32_t {
    if (const auto it = big_index_.find(id); it != big_index_.end()) {
      inserted = false;
      return kBig | it->second;
    }
    std::uint32_t i;
    if (!big_free_.empty()) {
      i = big_free_.back();
      big_free_.pop_back();
    } else {
      i = static_cast<std::uint32_t>(big_ids_.size());
      big_ids_.push_back(0);
      big_nodes_.push_back({});
    }
    big_ids_[i] = id;
    big_index_.emplace(id, i);
    inserted = true;
    return kBig | i;
  }
  [[gnu::noinline]] void erase_big(std::uint32_t r) {
    const std::uint32_t i = r & ~kBig;
    big_index_.erase(big_ids_[i]);
    big_free_.push_back(i);
  }

  struct Free {
    void operator()(Bucket* p) const { ::operator delete(p, std::align_val_t{64}); }
  };
  std::unique_ptr<Bucket, Free> mem_;
  Bucket* bk_ = nullptr;
  OrderNode* nodes_ = nullptr;           // bk_ viewed as a node array (see node())
  std::unique_ptr<std::uint8_t[]> occ_;  // per bucket: bit s = slot s in use
  std::size_t nb_ = 0, limit_ = 0, size_ = 0;
  Id next_new_ = 0;  // ids >= next_new_ were never inserted
  std::vector<std::uint32_t> remap_;  // after grow(): old ref -> new ref
  std::vector<Id> big_ids_;
  std::vector<OrderNode> big_nodes_;
  std::vector<std::uint32_t> big_free_;
  boost::unordered_flat_map<Id, std::uint32_t> big_index_;
};

}  // namespace lob::v19
