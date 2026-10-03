#pragma once
// Streaming reader for ITCH 5.0 files as distributed by NASDAQ: a sequence of
// [2-byte big-endian length][message]. Reads from any FILE* (a file, or stdin so
// that `zcat x.gz | book itch2ops -` never writes the ~10 GB raw file to disk).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace lob::itch {

class Reader {
 public:
  explicit Reader(std::FILE* f, std::size_t buffer_bytes = std::size_t{1} << 20);

  // Next message (without length prefix); valid until the next call.
  // Returns false at clean EOF; throws std::runtime_error on a truncated stream.
  auto next(std::span<const std::byte>& msg) -> bool;

  [[nodiscard]] auto messages() const noexcept -> std::uint64_t { return messages_; }
  [[nodiscard]] auto bytes() const noexcept -> std::uint64_t { return bytes_; }

 private:
  auto fill(std::size_t need) -> bool;  // ensure `need` bytes available at pos_

  std::FILE* f_;
  std::vector<std::byte> buf_;
  std::size_t pos_ = 0;
  std::size_t end_ = 0;
  std::uint64_t messages_ = 0;
  std::uint64_t bytes_ = 0;
};

}  // namespace lob::itch
