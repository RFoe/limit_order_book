#pragma once
// Output events and event sinks.
//
// Books are templates over a Sink and call sink.on(Event{...}) directly: no
// per-call vector allocation and no virtual dispatch in the measured path.
//  - RecordingSink: keeps every event (tests, differential testing)
//  - ChecksumSink:  folds events into a hash (benchmarks; defeats DCE and gives
//                   a cheap fingerprint to compare runs)

#include <lob/types.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace lob {

enum class RejectReason : std::uint8_t { InvalidQty, InvalidPrice, DuplicateId, UnknownId };

struct Trade {  // taker hit a resting maker at the maker's price
  OrderId taker;
  OrderId maker;
  Price price;
  Qty qty;
  friend auto operator==(const Trade&, const Trade&) -> bool = default;
};
struct Rested {  // (remaining) quantity was placed on the book
  OrderId id;
  Side side;
  Price price;
  Qty qty;
  friend auto operator==(const Rested&, const Rested&) -> bool = default;
};
struct Cancelled {  // order removed; qty = quantity that was still resting
  OrderId id;
  Qty qty;
  friend auto operator==(const Cancelled&, const Cancelled&) -> bool = default;
};
struct Reduced {  // modify that kept time priority
  OrderId id;
  Qty new_qty;
  friend auto operator==(const Reduced&, const Reduced&) -> bool = default;
};
struct Rejected {
  OrderId id;
  OpType op;
  RejectReason reason;
  friend auto operator==(const Rejected&, const Rejected&) -> bool = default;
};

using Event = std::variant<Trade, Rested, Cancelled, Reduced, Rejected>;

template <class S>
concept EventSink = requires(S& s) {
  s.on(Trade{});
  s.on(Rested{});
  s.on(Cancelled{});
  s.on(Reduced{});
  s.on(Rejected{});
};

struct RecordingSink {
  std::vector<Event> events;
  template <class E>
  void on(const E& e) {
    events.emplace_back(e);
  }
};

struct ChecksumSink {
  std::uint64_t value = 0;
  std::uint64_t events = 0;
  std::uint64_t trades = 0;

  void on(const Trade& e) noexcept {
    ++trades;
    mix(1, e.taker, e.maker, static_cast<std::uint64_t>(e.price), e.qty);
  }
  void on(const Rested& e) noexcept {
    mix(2, e.id, static_cast<std::uint64_t>(e.side), static_cast<std::uint64_t>(e.price), e.qty);
  }
  void on(const Cancelled& e) noexcept { mix(3, e.id, e.qty); }
  void on(const Reduced& e) noexcept { mix(4, e.id, e.new_qty); }
  void on(const Rejected& e) noexcept {
    mix(5, e.id, static_cast<std::uint64_t>(e.op), static_cast<std::uint64_t>(e.reason));
  }

 private:
  template <class... T>
  void mix(T... xs) noexcept {
    ++events;
    ((value = (value ^ static_cast<std::uint64_t>(xs)) * 0x9E3779B97F4A7C15ULL), ...);
  }
};

static_assert(EventSink<RecordingSink> && EventSink<ChecksumSink>);

auto to_string(Side s) -> std::string;
auto to_string(const Op& op) -> std::string;
auto to_string(const Event& e) -> std::string;

}  // namespace lob
