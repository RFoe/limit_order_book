#pragma once
// NASDAQ TotalView-ITCH 5.0: the subset of messages needed to rebuild an order
// book. All integers are big-endian; every message starts with
//   type(1) stock_locate(2) tracking_number(2) timestamp(6, ns since midnight).
// Field offsets/lengths follow the ITCH 5.0 specification.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace lob::itch {

template <class T>
inline auto read_be(const std::byte* p) noexcept -> T {
  T v;
  std::memcpy(&v, p, sizeof v);
  if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
  return v;
}

inline auto read_be48(const std::byte* p) noexcept -> std::uint64_t {
  std::uint64_t v = 0;
  for (int i = 0; i < 6; ++i) v = (v << 8) | static_cast<std::uint64_t>(p[i]);
  return v;
}

struct Header {
  char type;
  std::uint16_t stock_locate;
  std::uint16_t tracking;
  std::uint64_t timestamp;
};

struct SystemEvent {  // 'S', 12 bytes
  Header h;
  char event_code;  // O S Q M E C
};
struct StockDirectory {  // 'R', 39 bytes
  Header h;
  char stock[8];  // space padded
};
struct AddOrder {  // 'A' (36 bytes) and 'F' (40 bytes, + MPID attribution)
  Header h;
  std::uint64_t ref;
  char side;  // 'B' / 'S'
  std::uint32_t shares;
  char stock[8];
  std::uint32_t price;  // 1e-4 USD
};
struct OrderExecuted {  // 'E', 31 bytes
  Header h;
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
};
struct OrderExecutedWithPrice {  // 'C', 36 bytes
  Header h;
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
  char printable;
  std::uint32_t price;
};
struct OrderCancel {  // 'X', 23 bytes (partial cancel)
  Header h;
  std::uint64_t ref;
  std::uint32_t shares;
};
struct OrderDelete {  // 'D', 19 bytes
  Header h;
  std::uint64_t ref;
};
struct OrderReplace {  // 'U', 35 bytes
  Header h;
  std::uint64_t orig_ref;
  std::uint64_t new_ref;
  std::uint32_t shares;
  std::uint32_t price;
};

// Expected length of the message types we decode; 0 for everything else.
constexpr auto message_size(char type) noexcept -> std::size_t {
  switch (type) {
    case 'S': return 12;
    case 'R': return 39;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    default: return 0;
  }
}

inline auto parse_header(const std::byte* p) noexcept -> Header {
  return Header{static_cast<char>(p[0]), read_be<std::uint16_t>(p + 1), read_be<std::uint16_t>(p + 3),
                read_be48(p + 5)};
}

inline auto stock_view(const char (&stock)[8]) noexcept -> std::string_view {
  std::string_view s(stock, 8);
  return s.substr(0, s.find_last_not_of(' ') + 1);
}

// Decodes one message (without the 2-byte length prefix) and calls h.on(msg) for
// the types above, h.on_other(type) otherwise. Returns false if a known message
// is shorter than the spec says (malformed input).
template <class Handler>
auto dispatch(std::span<const std::byte> m, Handler& h) -> bool {
  if (m.empty()) return false;
  const char type = static_cast<char>(m[0]);
  const std::size_t need = message_size(type);
  if (need == 0) {
    h.on_other(type);
    return true;
  }
  if (m.size() < need) return false;
  const std::byte* p = m.data();
  const Header hdr = parse_header(p);
  switch (type) {
    case 'S':
      h.on(SystemEvent{hdr, static_cast<char>(p[11])});
      break;
    case 'R': {
      StockDirectory r{hdr, {}};
      std::memcpy(r.stock, p + 11, 8);
      h.on(r);
      break;
    }
    case 'A':
    case 'F': {
      AddOrder a{hdr, read_be<std::uint64_t>(p + 11), static_cast<char>(p[19]), read_be<std::uint32_t>(p + 20), {},
                 read_be<std::uint32_t>(p + 32)};
      std::memcpy(a.stock, p + 24, 8);
      h.on(a);
      break;
    }
    case 'E':
      h.on(OrderExecuted{hdr, read_be<std::uint64_t>(p + 11), read_be<std::uint32_t>(p + 19),
                         read_be<std::uint64_t>(p + 23)});
      break;
    case 'C':
      h.on(OrderExecutedWithPrice{hdr, read_be<std::uint64_t>(p + 11), read_be<std::uint32_t>(p + 19),
                                  read_be<std::uint64_t>(p + 23), static_cast<char>(p[31]),
                                  read_be<std::uint32_t>(p + 32)});
      break;
    case 'X':
      h.on(OrderCancel{hdr, read_be<std::uint64_t>(p + 11), read_be<std::uint32_t>(p + 19)});
      break;
    case 'D':
      h.on(OrderDelete{hdr, read_be<std::uint64_t>(p + 11)});
      break;
    case 'U':
      h.on(OrderReplace{hdr, read_be<std::uint64_t>(p + 11), read_be<std::uint64_t>(p + 19),
                        read_be<std::uint32_t>(p + 27), read_be<std::uint32_t>(p + 31)});
      break;
    default:
      break;
  }
  return true;
}

}  // namespace lob::itch
