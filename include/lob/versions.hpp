#pragma once
// The single registration point for book versions. Benchmarks, the differential
// test, the latency harness and the CLI all iterate over this list, so every
// version is compiled into the same binaries and can be A/B-ed in one process.
// Never delete or rewrite an old version: add include/lob/vN/ and append it here.

#include <lob/v0/book.hpp>
#include <lob/v1/book.hpp>
#include <lob/v2/book.hpp>
#include <lob/v3/book.hpp>
#include <lob/v4/book.hpp>
#include <lob/v5/book.hpp>
#include <lob/v7/book.hpp>
#include <lob/v8/book.hpp>
#include <lob/v9/book.hpp>
#include <lob/v10/book.hpp>
#include <lob/v11/book.hpp>
#include <lob/v12/book.hpp>
#include <lob/v13/book.hpp>
#include <lob/v14/book.hpp>

#include <string_view>
#include <tuple>

namespace lob {

template <template <class> class... Books>
struct VersionList {
  template <class Sink>
  using with_sink = std::tuple<Books<Sink>...>;
};

using Versions = VersionList<v0::Book, v1::Book, v2::Book, v3::Book, v4::Book, v5::Book, v7::Book, v8::Book, v9::Book, v10::Book, v11::Book, v12::Book, v13::Book, v14::Book>;

// Calls f.template operator()<Book>() for each Book in the tuple type.
template <class Tuple, class F>
constexpr void for_each_type(F&& f) {
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    (f.template operator()<std::tuple_element_t<I, Tuple>>(), ...);
  }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

}  // namespace lob
