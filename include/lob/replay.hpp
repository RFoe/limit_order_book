#pragma once
// Feeding an op stream into a book. In Debug builds (LOB_CHECK_INVARIANTS) every
// step is followed by check_invariants(); in Release this compiles away.

#include <lob/book_concept.hpp>
#include <lob/types.hpp>

#include <span>
#include <utility>

namespace lob {

#ifdef LOB_CHECK_INVARIANTS
inline constexpr bool kCheckInvariants = true;
#else
inline constexpr bool kCheckInvariants = false;
#endif

template <class Book>
inline void apply(Book& book, const Op& op) {
  switch (op.type) {
    case OpType::Push:
      book.push(op.id, op.side, op.price, op.qty);
      return;
    case OpType::Cancel:
      book.cancel(op.id);
      return;
    case OpType::Modify:
      book.modify(op.id, op.price, op.qty);
      return;
  }
  std::unreachable();
}

template <class Book>
void replay(Book& book, std::span<const Op> ops) {
  for (const Op& op : ops) {
    apply(book, op);
    if constexpr (kCheckInvariants) book.check_invariants();
  }
}

}  // namespace lob
