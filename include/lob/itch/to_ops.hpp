#pragma once
// ITCH 5.0 -> lob::Op conversion for a set of symbols.
//
// Mapping (the exchange already did the matching, so adds never cross):
//   A/F add            -> push(ref, side, price, shares)
//   E/C executed, X partial cancel
//                      -> modify(ref, same price, remaining)   (reduce: keeps priority)
//                         or cancel(ref) when nothing remains
//   D delete           -> cancel(ref)
//   U replace          -> cancel(orig_ref) + push(new_ref, same side, new price, new shares)
// Replaying the result exercises the add / cancel / reduce paths; the matching
// path is barely touched (validate with `book validate`, which counts trades).

#include <lob/itch/messages.hpp>
#include <lob/types.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lob::itch {

struct SymbolStats {
  std::uint64_t adds = 0;
  std::uint64_t executions = 0;  // E + C
  std::uint64_t cancels = 0;     // X
  std::uint64_t deletes = 0;     // D
  std::uint64_t replaces = 0;    // U
  std::uint64_t overfills = 0;   // executed/cancelled more than tracked qty (should be 0)
};

class ToOps {
 public:
  explicit ToOps(std::vector<std::string> symbols);

  void on(const SystemEvent&) {}
  void on(const StockDirectory& m);
  void on(const AddOrder& m);
  void on(const OrderExecuted& m) { reduce(m.ref, m.shares, &SymbolStats::executions); }
  void on(const OrderExecutedWithPrice& m) { reduce(m.ref, m.shares, &SymbolStats::executions); }
  void on(const OrderCancel& m) { reduce(m.ref, m.shares, &SymbolStats::cancels); }
  void on(const OrderDelete& m);
  void on(const OrderReplace& m);
  void on_other(char type) { ++other_[static_cast<unsigned char>(type)]; }

  [[nodiscard]] auto symbols() const -> const std::vector<std::string>& { return symbols_; }
  [[nodiscard]] auto ops(std::size_t i) const -> const std::vector<Op>& { return ops_[i]; }
  [[nodiscard]] auto stats(std::size_t i) const -> const SymbolStats& { return stats_[i]; }
  // adds per stock locate, for picking the most active symbols
  [[nodiscard]] auto adds_by_locate() const -> const std::vector<std::uint64_t>& { return adds_by_locate_; }
  [[nodiscard]] auto symbol_of_locate(std::uint16_t locate) const -> std::string_view;

 private:
  struct Tracked {
    std::uint32_t sym;
    Side side;
    Price price;
    Qty qty;
  };

  auto wanted(std::uint16_t locate, const char (&stock)[8]) -> int;
  void reduce(std::uint64_t ref, std::uint32_t shares, std::uint64_t SymbolStats::*counter);

  std::vector<std::string> symbols_;
  std::vector<std::vector<Op>> ops_;
  std::vector<SymbolStats> stats_;
  std::vector<int> locate_to_sym_;            // -2 unknown, -1 not wanted
  std::vector<std::string> locate_names_;
  std::vector<std::uint64_t> adds_by_locate_;
  std::unordered_map<std::uint64_t, Tracked> orders_;
  std::array<std::uint64_t, 256> other_{};
};

}  // namespace lob::itch
