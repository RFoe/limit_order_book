#pragma once
// Binary workload file: a 256-byte header followed by n_ops raw `Op` records.
// Loaded in one read() into memory before any measurement, so I/O and
// generation never show up in benchmark numbers. Little-endian only.

#include <lob/types.hpp>

#include <bit>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lob::workload {

static_assert(std::endian::native == std::endian::little, "workload files are little-endian");

enum class Source : std::uint32_t { Synthetic = 0, Itch = 1 };

inline constexpr char kMagic[8] = {'L', 'O', 'B', 'O', 'P', 'S', '0', '1'};
inline constexpr std::uint32_t kVersion = 1;

struct FileHeader {
  char magic[8];
  std::uint32_t version;
  Source source;
  std::uint64_t n_ops;
  std::uint64_t checksum;  // FNV-1a over the op bytes
  std::uint64_t seed;      // synthetic only
  char description[216];   // NUL-terminated, human readable parameters
};
static_assert(sizeof(FileHeader) == 256);

struct Workload {
  FileHeader header{};
  std::vector<Op> ops;
  [[nodiscard]] auto description() const -> std::string_view { return header.description; }
};

auto checksum(std::span<const Op> ops) -> std::uint64_t;

auto save(const std::filesystem::path& path, Source source, std::uint64_t seed, std::string_view description,
          std::span<const Op> ops) -> std::expected<void, std::string>;

auto load(const std::filesystem::path& path) -> std::expected<Workload, std::string>;

auto to_string(Source s) -> std::string_view;

}  // namespace lob::workload
