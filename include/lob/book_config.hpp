#pragma once
// Per-instrument reference data a book may need, and a factory that passes it
// only to books that accept it (v0..v2 are constructed from the sink alone).

#include <lob/types.hpp>

#include <concepts>
#include <memory>
#include <span>

namespace lob {

struct BookConfig {
  Price tick = 1;  // price grid; prices off the grid are legal (books must still handle them)
};

template <class Book, class Sink>
auto make_book(Sink& sink, const BookConfig& cfg) -> std::unique_ptr<Book> {
  if constexpr (std::constructible_from<Book, Sink&, const BookConfig&>)
    return std::make_unique<Book>(sink, cfg);
  else
    return std::make_unique<Book>(sink);
}

// Stand-in for instrument reference data: the largest power of ten that is a
// multiple of >= 99.9% of all prices in the stream (US equities >= $1 quote in
// $0.01 = 100 ITCH units; the synthetic generator uses 1).
auto infer_tick(std::span<const Op> ops) -> Price;

}  // namespace lob
