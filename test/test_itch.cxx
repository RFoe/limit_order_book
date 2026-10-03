// ITCH 5.0 parser / converter tests on hand-built big-endian byte streams.

#include <lob/itch/messages.hpp>
#include <lob/itch/parser.hpp>
#include <lob/itch/to_ops.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace lob;

struct Writer {
  std::vector<std::byte> b;
  void c(char x) { b.push_back(static_cast<std::byte>(x)); }
  void be(std::uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
  }
  void stock(std::string_view s) {
    for (std::size_t i = 0; i < 8; ++i) c(i < s.size() ? s[i] : ' ');
  }
  void header(char type, std::uint16_t locate) {
    c(type);
    be(locate, 2);
    be(0, 2);           // tracking
    be(34'200'000'000'000ULL, 6);  // 09:30:00 in ns
  }
};

// framed stream: [u16 length][message]...
struct Stream {
  std::vector<std::byte> bytes;
  void add(const Writer& m) {
    bytes.push_back(static_cast<std::byte>(m.b.size() >> 8));
    bytes.push_back(static_cast<std::byte>(m.b.size() & 0xff));
    bytes.insert(bytes.end(), m.b.begin(), m.b.end());
  }
};

auto directory(std::uint16_t locate, std::string_view sym) -> Writer {
  Writer w;
  w.header('R', locate);
  w.stock(sym);
  for (int i = 0; i < 39 - 19; ++i) w.c('\0');  // remaining directory fields
  return w;
}
auto add(std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares, std::string_view sym,
         std::uint32_t price, bool mpid = false) -> Writer {
  Writer w;
  w.header(mpid ? 'F' : 'A', locate);
  w.be(ref, 8);
  w.c(side);
  w.be(shares, 4);
  w.stock(sym);
  w.be(price, 4);
  if (mpid) w.stock("MPID");  // 4 bytes attribution
  if (mpid) w.b.resize(40);
  return w;
}
auto executed(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) -> Writer {
  Writer w;
  w.header('E', locate);
  w.be(ref, 8);
  w.be(shares, 4);
  w.be(777, 8);
  return w;
}
auto executed_px(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares, std::uint32_t px) -> Writer {
  Writer w;
  w.header('C', locate);
  w.be(ref, 8);
  w.be(shares, 4);
  w.be(778, 8);
  w.c('Y');
  w.be(px, 4);
  return w;
}
auto cancel(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) -> Writer {
  Writer w;
  w.header('X', locate);
  w.be(ref, 8);
  w.be(shares, 4);
  return w;
}
auto del(std::uint16_t locate, std::uint64_t ref) -> Writer {
  Writer w;
  w.header('D', locate);
  w.be(ref, 8);
  return w;
}
auto replace(std::uint16_t locate, std::uint64_t orig, std::uint64_t next, std::uint32_t shares, std::uint32_t px)
    -> Writer {
  Writer w;
  w.header('U', locate);
  w.be(orig, 8);
  w.be(next, 8);
  w.be(shares, 4);
  w.be(px, 4);
  return w;
}

struct TmpFile {
  std::FILE* f = std::tmpfile();
  ~TmpFile() { std::fclose(f); }
  explicit TmpFile(const std::vector<std::byte>& data) {
    std::fwrite(data.data(), 1, data.size(), f);
    std::rewind(f);
  }
};

auto convert(const Stream& s, std::vector<std::string> symbols) -> std::unique_ptr<itch::ToOps> {
  TmpFile tmp(s.bytes);
  itch::Reader reader(tmp.f);
  auto conv = std::make_unique<itch::ToOps>(std::move(symbols));
  for (std::span<const std::byte> m; reader.next(m);) REQUIRE(itch::dispatch(m, *conv));
  return conv;
}

}  // namespace

TEST_CASE("message sizes match the ITCH 5.0 spec", "[itch]") {
  CHECK(add(1, 1, 'B', 1, "X", 1).b.size() == 36);
  CHECK(add(1, 1, 'B', 1, "X", 1, true).b.size() == 40);
  CHECK(executed(1, 1, 1).b.size() == 31);
  CHECK(executed_px(1, 1, 1, 1).b.size() == 36);
  CHECK(cancel(1, 1, 1).b.size() == 23);
  CHECK(del(1, 1).b.size() == 19);
  CHECK(replace(1, 1, 2, 1, 1).b.size() == 35);
  CHECK(directory(1, "X").b.size() == 39);
}

namespace {
struct LastAdd {
  itch::AddOrder last{};
  int adds = 0;
  void on(const itch::AddOrder& a) {
    last = a;
    ++adds;
  }
  void on(const auto&) {}
  void on_other(char) {}
};
}  // namespace

TEST_CASE("add order fields are decoded big-endian", "[itch]") {
  LastAdd h;
  const auto m = add(0x0102, 0x1122334455667788ULL, 'S', 0xA0B0C0D0u, "AAPL", 1'234'5678);
  REQUIRE(itch::dispatch(m.b, h));
  REQUIRE(h.adds == 1);
  CHECK(h.last.h.type == 'A');
  CHECK(h.last.h.stock_locate == 0x0102);
  CHECK(h.last.h.timestamp == 34'200'000'000'000ULL);
  CHECK(h.last.ref == 0x1122334455667788ULL);
  CHECK(h.last.side == 'S');
  CHECK(h.last.shares == 0xA0B0C0D0u);
  CHECK(itch::stock_view(h.last.stock) == "AAPL");
  CHECK(h.last.price == 1'234'5678u);

  Writer truncated = m;
  truncated.b.resize(30);
  CHECK_FALSE(itch::dispatch(truncated.b, h));
}

TEST_CASE("converter maps the order lifecycle to push/modify/cancel", "[itch]") {
  Stream s;
  s.add(directory(7, "AAPL"));
  s.add(directory(8, "MSFT"));
  s.add(add(7, 1, 'B', 100, "AAPL", 1'500'000));
  s.add(add(8, 2, 'S', 50, "MSFT", 2'000'000, true));  // other symbol: ignored
  s.add(executed(7, 1, 30));                           // 100 -> 70
  s.add(cancel(7, 1, 20));                             // 70 -> 50
  s.add(replace(7, 1, 3, 40, 1'510'000));              // cancel 1, push 3
  s.add(executed_px(7, 3, 40, 1'510'000));             // fully executed -> cancel
  s.add(add(7, 4, 'S', 10, "AAPL", 1'600'000));
  s.add(del(7, 4));
  s.add(executed(8, 2, 50));  // other symbol
  Writer unknown;
  unknown.header('Z', 0);  // unknown message type: skipped
  s.add(unknown);

  const auto conv = convert(s, {"AAPL"});
  const std::vector<Op> expected = {
      make_push(1, Side::Buy, 1'500'000, 100), make_modify(1, 1'500'000, 70),
      make_modify(1, 1'500'000, 50),           make_cancel(1),
      make_push(3, Side::Buy, 1'510'000, 40),  make_cancel(3),
      make_push(4, Side::Sell, 1'600'000, 10), make_cancel(4),
  };
  CHECK(conv->ops(0) == expected);
  const auto& st = conv->stats(0);
  CHECK(st.adds == 2);
  CHECK(st.executions == 2);
  CHECK(st.cancels == 1);
  CHECK(st.deletes == 1);
  CHECK(st.replaces == 1);
  CHECK(st.overfills == 0);
  CHECK(conv->adds_by_locate()[8] == 1);
  CHECK(conv->symbol_of_locate(8) == "MSFT");
}

TEST_CASE("reader handles streams larger than its buffer", "[itch]") {
  Stream s;
  s.add(directory(7, "AAPL"));
  const int n = 10'000;  // ~380 KB, several buffer refills
  for (int i = 1; i <= n; ++i) {
    s.add(add(7, static_cast<std::uint64_t>(i), 'B', 100, "AAPL", 1'000'000));
    s.add(del(7, static_cast<std::uint64_t>(i)));
  }
  const auto conv = convert(s, {"AAPL"});
  CHECK(conv->ops(0).size() == 2 * n);
  CHECK(conv->stats(0).adds == n);
  CHECK(conv->stats(0).deletes == n);
}

TEST_CASE("reader rejects a truncated stream", "[itch]") {
  Stream s;
  s.add(add(7, 1, 'B', 100, "AAPL", 1'000'000));
  s.bytes.resize(s.bytes.size() - 5);
  TmpFile tmp(s.bytes);
  itch::Reader reader(tmp.f);
  std::span<const std::byte> m;
  CHECK_THROWS_AS(reader.next(m), std::runtime_error);
}
