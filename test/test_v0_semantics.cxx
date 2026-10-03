// Hand-written scenarios pinning down the matching semantics of v0 (the
// reference every other version is diffed against).

#include <lob/events.hpp>
#include <lob/v0/book.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

using namespace lob;
using Events = std::vector<Event>;
constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

struct Fixture {
  RecordingSink sink;
  v0::Book<RecordingSink> book{sink};

  auto push(OrderId id, Side side, Price px, Qty qty) -> Events {
    book.push(id, side, px, qty);
    return take();
  }
  auto cancel(OrderId id) -> Events {
    book.cancel(id);
    return take();
  }
  auto modify(OrderId id, Price px, Qty qty) -> Events {
    book.modify(id, px, qty);
    return take();
  }
  auto take() -> Events {
    book.check_invariants();
    Events e = std::move(sink.events);
    sink.events.clear();
    return e;
  }
};

}  // namespace

TEST_CASE("passive orders rest and set best prices", "[v0]") {
  Fixture f;
  CHECK(f.push(1, B, 99, 10) == Events{Rested{1, B, 99, 10}});
  CHECK(f.push(2, S, 101, 5) == Events{Rested{2, S, 101, 5}});
  CHECK(f.push(3, B, 99, 7) == Events{Rested{3, B, 99, 7}});
  CHECK(f.book.best_bid() == 99);
  CHECK(f.book.best_ask() == 101);
  CHECK(f.book.order_count() == 3);
  CHECK(f.book.levels(B) == std::vector<LevelSnapshot>{{99, 17, 2}});
  CHECK(f.book.levels(S) == std::vector<LevelSnapshot>{{101, 5, 1}});
}

TEST_CASE("a crossing order trades at the maker's price", "[v0]") {
  Fixture f;
  f.push(1, S, 100, 10);
  CHECK(f.push(2, B, 105, 4) == Events{Trade{2, 1, 100, 4}});
  CHECK(f.book.levels(S) == std::vector<LevelSnapshot>{{100, 6, 1}});
  CHECK(f.book.best_bid() == std::nullopt);
  // sell side taker against a bid
  f.push(3, B, 98, 5);
  CHECK(f.push(4, S, 90, 2) == Events{Trade{4, 3, 98, 2}});
}

TEST_CASE("a large order sweeps several levels and rests the remainder", "[v0]") {
  Fixture f;
  f.push(1, S, 100, 5);
  f.push(2, S, 101, 5);
  f.push(3, S, 102, 5);
  CHECK(f.push(4, B, 101, 12) == Events{Trade{4, 1, 100, 5}, Trade{4, 2, 101, 5}, Rested{4, B, 101, 2}});
  CHECK(f.book.best_bid() == 101);
  CHECK(f.book.best_ask() == 102);
  CHECK(f.book.order_count() == 2);
}

TEST_CASE("FIFO within a price level", "[v0]") {
  Fixture f;
  f.push(1, S, 100, 5);
  f.push(2, S, 100, 5);
  f.push(3, S, 100, 5);
  CHECK(f.push(4, B, 100, 7) == Events{Trade{4, 1, 100, 5}, Trade{4, 2, 100, 2}});
  CHECK(f.book.levels(S) == std::vector<LevelSnapshot>{{100, 8, 2}});
}

TEST_CASE("cancel removes the order and reports the remaining quantity", "[v0]") {
  Fixture f;
  f.push(1, S, 100, 10);
  f.push(2, B, 100, 4);  // partial fill of 1
  CHECK(f.cancel(1) == Events{Cancelled{1, 6}});
  CHECK(f.book.order_count() == 0);
  CHECK(f.book.best_ask() == std::nullopt);
  CHECK(f.cancel(1) == Events{Rejected{1, OpType::Cancel, RejectReason::UnknownId}});
}

TEST_CASE("reducing quantity at the same price keeps time priority", "[v0]") {
  Fixture f;
  f.push(1, S, 100, 5);
  f.push(2, S, 100, 5);
  CHECK(f.modify(1, 100, 3) == Events{Reduced{1, 3}});
  CHECK(f.book.levels(S) == std::vector<LevelSnapshot>{{100, 8, 2}});
  CHECK(f.push(3, B, 100, 4) == Events{Trade{3, 1, 100, 3}, Trade{3, 2, 100, 1}});
}

TEST_CASE("modify to the same quantity is a priority-keeping no-op", "[v0]") {
  Fixture f;
  f.push(1, S, 100, 5);
  f.push(2, S, 100, 5);
  CHECK(f.modify(1, 100, 5) == Events{Reduced{1, 5}});
  CHECK(f.push(3, B, 100, 5) == Events{Trade{3, 1, 100, 5}});
}

TEST_CASE("increasing quantity loses time priority", "[v0]") {
  Fixture f;
  f.push(1, S, 100, 5);
  f.push(2, S, 100, 5);
  CHECK(f.modify(1, 100, 6) == Events{Cancelled{1, 5}, Rested{1, S, 100, 6}});
  CHECK(f.push(3, B, 100, 5) == Events{Trade{3, 2, 100, 5}});
}

TEST_CASE("repricing loses priority and may cross", "[v0]") {
  Fixture f;
  f.push(1, B, 99, 5);
  f.push(2, S, 101, 5);
  f.push(3, S, 102, 5);
  // move 3 in front of 2 at a better price: no longer behind 2
  CHECK(f.modify(3, 100, 5) == Events{Cancelled{3, 5}, Rested{3, S, 100, 5}});
  CHECK(f.book.best_ask() == 100);
  // reprice through the bid: cancel, then behave exactly like a fresh push
  CHECK(f.modify(2, 99, 3) == Events{Cancelled{2, 5}, Trade{2, 1, 99, 3}});
  CHECK(f.book.levels(B) == std::vector<LevelSnapshot>{{99, 2, 1}});
}

TEST_CASE("duplicate live ids are rejected, filled ids can be reused", "[v0]") {
  Fixture f;
  f.push(1, B, 99, 5);
  CHECK(f.push(1, S, 120, 5) == Events{Rejected{1, OpType::Push, RejectReason::DuplicateId}});
  f.push(2, S, 99, 5);  // fills 1 completely
  CHECK(f.book.order_count() == 0);
  CHECK(f.push(1, S, 120, 5) == Events{Rested{1, S, 120, 5}});
}

TEST_CASE("invalid input is rejected without touching the book", "[v0]") {
  Fixture f;
  f.push(1, B, 99, 5);
  CHECK(f.push(2, B, 99, 0) == Events{Rejected{2, OpType::Push, RejectReason::InvalidQty}});
  CHECK(f.push(3, B, 0, 5) == Events{Rejected{3, OpType::Push, RejectReason::InvalidPrice}});
  CHECK(f.push(4, B, -1, 5) == Events{Rejected{4, OpType::Push, RejectReason::InvalidPrice}});
  CHECK(f.modify(9, 99, 5) == Events{Rejected{9, OpType::Modify, RejectReason::UnknownId}});
  CHECK(f.modify(1, 99, 0) == Events{Rejected{1, OpType::Modify, RejectReason::InvalidQty}});
  CHECK(f.modify(1, 0, 5) == Events{Rejected{1, OpType::Modify, RejectReason::InvalidPrice}});
  CHECK(f.book.levels(B) == std::vector<LevelSnapshot>{{99, 5, 1}});
}

TEST_CASE("a fully filled taker does not rest", "[v0]") {
  Fixture f;
  f.push(1, S, 100, 5);
  CHECK(f.push(2, B, 100, 5) == Events{Trade{2, 1, 100, 5}});
  CHECK(f.book.order_count() == 0);
  CHECK(f.book.levels(S).empty());
  CHECK(f.book.levels(B).empty());
}
