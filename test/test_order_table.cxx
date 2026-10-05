// v19 OrderTable: lookup / insert / erase against std::unordered_map (ids in
// the 32-bit key range, at its edge, and >= 2^32 for the big-id pool), and the
// node links across growth: the nodes form one doubly linked list whose head /
// tail the test holds outside the table (like a price level), re-mapped after
// every grow; the list must keep its order and back links.

#include <lob/v19/order_table.hpp>
#include <lob/workload/generator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

using lob::v19::OrderNode;
using lob::v19::OrderTable;

TEST_CASE("OrderTable matches std::unordered_map and keeps node links across growth", "[v19][order_table]") {
  OrderTable t;
  std::unordered_map<std::uint64_t, std::uint32_t> ref;  // id -> qty stored in its node
  std::list<std::uint64_t> order;                        // the linked list, by id, head first
  std::uint32_t head = OrderTable::kNil, tail = OrderTable::kNil;
  lob::workload::Rng rng(19);
  std::uint64_t mono = 1'000'000;
  std::vector<std::uint64_t> keys;

  auto relink = [&] {
    if (!t.grew()) return;
    head = t.remap(head);
    tail = t.remap(tail);
    t.clear_remap();
  };
  auto append = [&](std::uint32_t r) {  // push_back onto the external list
    OrderNode& n = t.node(r);
    n.prev = tail;
    n.next = OrderTable::kNil;
    if (tail != OrderTable::kNil)
      t.node(tail).next = r;
    else
      head = r;
    tail = r;
  };
  auto unlink = [&](std::uint32_t r) {
    const OrderNode n = t.node(r);
    if (n.prev != OrderTable::kNil) t.node(n.prev).next = n.next; else head = n.next;
    if (n.next != OrderTable::kNil) t.node(n.next).prev = n.prev; else tail = n.prev;
  };
  auto draw = [&]() -> std::uint64_t {
    const auto x = rng.uniform(100);
    if (x < 70) return mono += 1 + rng.uniform(30);                          // new, increasing (fast path)
    if (x < 85) return keys.empty() ? mono : keys[rng.uniform(keys.size())];  // existing / erased
    if (x < 92) return mono - rng.uniform(100'000);                          // older id
    if (x < 96) return (1ULL << 33) + rng.uniform(500);                      // big-id pool
    return 0xFFFF'FFFEULL + rng.uniform(3);                                  // 32-bit edge
  };

  for (int step = 0; step < 300'000; ++step) {
    const std::uint64_t id = draw();
    if (rng.uniform(10) < 6) {  // insert-if-absent, then append its node to the list
      bool inserted = false;
      const std::uint32_t r = t.try_insert(id, inserted);
      relink();
      REQUIRE(inserted == !ref.contains(id));
      if (inserted) {
        const auto qty = static_cast<std::uint32_t>(rng.uniform(1000) + 1);
        t.node(r).qty = qty;
        append(r);
        ref[id] = qty;
        order.push_back(id);
        keys.push_back(id);
      } else {
        REQUIRE(t.node(r).qty == ref.at(id));
      }
    } else {  // find + unlink + erase
      const std::uint32_t r = t.find(id);
      REQUIRE((r != OrderTable::kNil) == ref.contains(id));
      if (r != OrderTable::kNil) {
        REQUIRE(t.id_of(r) == id);
        REQUIRE(t.node(r).qty == ref.at(id));
        unlink(r);
        t.erase(r);
        ref.erase(id);
        order.erase(std::find(order.begin(), order.end(), id));
      }
    }
    REQUIRE(t.size() == ref.size());
    if (step % 20'000 == 0) {  // full walk: order, back links, ids, quantities
      std::uint32_t prev = OrderTable::kNil;
      auto it = order.begin();
      for (std::uint32_t r = head; r != OrderTable::kNil; prev = r, r = t.node(r).next, ++it) {
        REQUIRE(it != order.end());
        REQUIRE(t.id_of(r) == *it);
        REQUIRE(t.node(r).prev == prev);
        REQUIRE(t.node(r).qty == ref.at(*it));
        REQUIRE(t.find(*it) == r);
      }
      REQUIRE(it == order.end());
      REQUIRE(prev == tail);
    }
  }
  CHECK(t.buckets() > 256);  // it did grow
}
