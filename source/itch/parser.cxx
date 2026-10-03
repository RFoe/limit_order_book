#include <lob/itch/messages.hpp>
#include <lob/itch/parser.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace lob::itch {

// the buffer must hold the largest possible frame (2 + 65535 bytes)
Reader::Reader(std::FILE* f, std::size_t buffer_bytes)
    : f_(f), buf_(std::max<std::size_t>(buffer_bytes, 2 + 65535)) {}

auto Reader::fill(std::size_t need) -> bool {
  if (end_ - pos_ >= need) return true;
  // compact, then read more
  std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
  end_ -= pos_;
  pos_ = 0;
  while (end_ < need) {
    const std::size_t n = std::fread(buf_.data() + end_, 1, buf_.size() - end_, f_);
    if (n == 0) return false;
    end_ += n;
  }
  return true;
}

auto Reader::next(std::span<const std::byte>& msg) -> bool {
  if (!fill(2)) {
    if (end_ != pos_) throw std::runtime_error("itch: truncated length prefix");
    return false;
  }
  const std::size_t len = read_be<std::uint16_t>(buf_.data() + pos_);
  if (!fill(2 + len)) throw std::runtime_error("itch: truncated message");
  msg = std::span<const std::byte>(buf_.data() + pos_ + 2, len);
  pos_ += 2 + len;
  bytes_ += 2 + len;
  ++messages_;
  return true;
}

}  // namespace lob::itch
