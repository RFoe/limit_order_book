#include <lob/events.hpp>

#include <format>
#include <string>
#include <variant>

namespace lob {

namespace {

auto to_string(OpType t) -> std::string_view {
  switch (t) {
    case OpType::Push: return "push";
    case OpType::Cancel: return "cancel";
    case OpType::Modify: return "modify";
  }
  return "?";
}

auto to_string(RejectReason r) -> std::string_view {
  switch (r) {
    case RejectReason::InvalidQty: return "invalid_qty";
    case RejectReason::InvalidPrice: return "invalid_price";
    case RejectReason::DuplicateId: return "duplicate_id";
    case RejectReason::UnknownId: return "unknown_id";
  }
  return "?";
}

}  // namespace

auto to_string(Side s) -> std::string { return s == Side::Buy ? "buy" : "sell"; }

auto to_string(const Op& op) -> std::string {
  switch (op.type) {
    case OpType::Push: return std::format("push(id={}, {}, px={}, qty={})", op.id, to_string(op.side), op.price, op.qty);
    case OpType::Cancel: return std::format("cancel(id={})", op.id);
    case OpType::Modify: return std::format("modify(id={}, px={}, qty={})", op.id, op.price, op.qty);
  }
  return "?";
}

auto to_string(const Event& e) -> std::string {
  struct Visitor {
    auto operator()(const Trade& t) const -> std::string {
      return std::format("Trade(taker={}, maker={}, px={}, qty={})", t.taker, t.maker, t.price, t.qty);
    }
    auto operator()(const Rested& r) const -> std::string {
      return std::format("Rested(id={}, {}, px={}, qty={})", r.id, to_string(r.side), r.price, r.qty);
    }
    auto operator()(const Cancelled& c) const -> std::string {
      return std::format("Cancelled(id={}, qty={})", c.id, c.qty);
    }
    auto operator()(const Reduced& r) const -> std::string {
      return std::format("Reduced(id={}, qty={})", r.id, r.new_qty);
    }
    auto operator()(const Rejected& r) const -> std::string {
      return std::format("Rejected(id={}, op={}, {})", r.id, to_string(r.op), to_string(r.reason));
    }
  };
  return std::visit(Visitor{}, e);
}

}  // namespace lob
