#include <lob/workload/format.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>

namespace lob::workload {

namespace {

struct FileCloser {
  void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

}  // namespace

auto checksum(std::span<const Op> ops) -> std::uint64_t {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::byte b : std::as_bytes(ops)) {
    h ^= static_cast<std::uint64_t>(b);
    h *= 0x100000001b3ULL;
  }
  return h;
}

auto to_string(Source s) -> std::string_view {
  switch (s) {
    case Source::Synthetic: return "synthetic";
    case Source::Itch: return "itch";
  }
  return "?";
}

auto save(const std::filesystem::path& path, Source source, std::uint64_t seed, std::string_view description,
          std::span<const Op> ops) -> std::expected<void, std::string> {
  FileHeader h{};
  std::memcpy(h.magic, kMagic, sizeof kMagic);
  h.version = kVersion;
  h.source = source;
  h.n_ops = ops.size();
  h.checksum = checksum(ops);
  h.seed = seed;
  const auto n = std::min(description.size(), sizeof h.description - 1);
  std::memcpy(h.description, description.data(), n);

  File f{std::fopen(path.c_str(), "wb")};
  if (!f) return std::unexpected(std::format("cannot open {} for writing", path.string()));
  if (std::fwrite(&h, sizeof h, 1, f.get()) != 1 ||
      (!ops.empty() && std::fwrite(ops.data(), sizeof(Op), ops.size(), f.get()) != ops.size()))
    return std::unexpected(std::format("short write to {}", path.string()));
  return {};
}

auto load(const std::filesystem::path& path) -> std::expected<Workload, std::string> {
  File f{std::fopen(path.c_str(), "rb")};
  if (!f) return std::unexpected(std::format("cannot open {}", path.string()));
  Workload w;
  if (std::fread(&w.header, sizeof w.header, 1, f.get()) != 1)
    return std::unexpected(std::format("{}: truncated header", path.string()));
  if (std::memcmp(w.header.magic, kMagic, sizeof kMagic) != 0)
    return std::unexpected(std::format("{}: bad magic", path.string()));
  if (w.header.version != kVersion)
    return std::unexpected(std::format("{}: unsupported version {}", path.string(), w.header.version));
  w.header.description[sizeof w.header.description - 1] = '\0';
  w.ops.resize(w.header.n_ops);
  if (w.header.n_ops != 0 && std::fread(w.ops.data(), sizeof(Op), w.ops.size(), f.get()) != w.ops.size())
    return std::unexpected(std::format("{}: truncated ops", path.string()));
  if (checksum(w.ops) != w.header.checksum) return std::unexpected(std::format("{}: checksum mismatch", path.string()));
  return w;
}

}  // namespace lob::workload
