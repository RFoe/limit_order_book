#include <lob/itch/to_ops.hpp>

#include <algorithm>
#include <utility>

namespace lob::itch {

ToOps::ToOps(std::vector<std::string> symbols)
    : symbols_(std::move(symbols)),
      ops_(symbols_.size()),
      stats_(symbols_.size()),
      locate_to_sym_(65536, -2),
      locate_names_(65536),
      adds_by_locate_(65536, 0) {}

auto ToOps::symbol_of_locate(std::uint16_t locate) const -> std::string_view { return locate_names_[locate]; }

auto ToOps::wanted(std::uint16_t locate, const char (&stock)[8]) -> int {
  int& slot = locate_to_sym_[locate];
  if (slot == -2) {
    const std::string_view name = stock_view(stock);
    locate_names_[locate] = name;
    const auto it = std::ranges::find(symbols_, name);
    slot = it == symbols_.end() ? -1 : static_cast<int>(it - symbols_.begin());
  }
  return slot;
}

void ToOps::on(const StockDirectory& m) {
  locate_to_sym_[m.h.stock_locate] = -2;  // (re)resolve from the directory entry
  wanted(m.h.stock_locate, m.stock);
}

void ToOps::on(const AddOrder& m) {
  ++adds_by_locate_[m.h.stock_locate];
  const int sym = wanted(m.h.stock_locate, m.stock);
  if (sym < 0) return;
  const Side side = m.side == 'B' ? Side::Buy : Side::Sell;
  const auto price = static_cast<Price>(m.price);
  orders_[m.ref] = Tracked{static_cast<std::uint32_t>(sym), side, price, m.shares};
  ops_[sym].push_back(make_push(m.ref, side, price, m.shares));
  ++stats_[sym].adds;
}

void ToOps::reduce(std::uint64_t ref, std::uint32_t shares, std::uint64_t SymbolStats::*counter) {
  auto it = orders_.find(ref);
  if (it == orders_.end()) return;  // other symbol
  Tracked& o = it->second;
  SymbolStats& st = stats_[o.sym];
  ++(st.*counter);
  if (shares >= o.qty) {
    if (shares > o.qty) ++st.overfills;
    ops_[o.sym].push_back(make_cancel(ref));
    orders_.erase(it);
    return;
  }
  o.qty -= shares;
  ops_[o.sym].push_back(make_modify(ref, o.price, o.qty));
}

void ToOps::on(const OrderDelete& m) {
  auto it = orders_.find(m.ref);
  if (it == orders_.end()) return;
  ++stats_[it->second.sym].deletes;
  ops_[it->second.sym].push_back(make_cancel(m.ref));
  orders_.erase(it);
}

void ToOps::on(const OrderReplace& m) {
  auto it = orders_.find(m.orig_ref);
  if (it == orders_.end()) return;
  const Tracked old = it->second;
  orders_.erase(it);
  ++stats_[old.sym].replaces;
  auto& ops = ops_[old.sym];
  ops.push_back(make_cancel(m.orig_ref));
  const auto price = static_cast<Price>(m.price);
  orders_[m.new_ref] = Tracked{old.sym, old.side, price, m.shares};
  ops.push_back(make_push(m.new_ref, old.side, price, m.shares));
}

}  // namespace lob::itch
