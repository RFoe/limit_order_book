#pragma once
// One-shot, zero-filled, pre-faulted anonymous memory, backed by 2 MiB pages
// when possible (fewer TLB entries for a MB-sized index). Policy, in order:
//   hugetlb : mmap(MAP_HUGETLB | MAP_POPULATE) from the reserved pool
//             (vm.nr_hugepages, see scripts/env_tune.sh)
//   thp     : 2 MiB-aligned mmap + madvise(MADV_HUGEPAGE) + MADV_POPULATE_WRITE
//   small   : 4 KiB pages (MADV_NOHUGEPAGE), pre-faulted
// LOB_HUGE=auto|hugetlb|thp|off selects the starting point (auto = hugetlb);
// it is how the hugepage effect is measured separately from the data layout.
// huge_bytes() reports what the kernel actually backs with huge pages
// (/proc/self/smaps), since THP may silently fall back to small pages.

#include <cstddef>
#include <string_view>

namespace lob::mem {

enum class PageMode : unsigned char { HugeTLB, THP, Small };

auto to_string(PageMode m) -> std::string_view;

inline constexpr std::size_t kHugePage = std::size_t{2} << 20;

class HugeBuffer {
  public:
    HugeBuffer() = default;
    explicit HugeBuffer(std::size_t bytes); // rounded up to 2 MiB
    ~HugeBuffer();
    HugeBuffer(HugeBuffer &&other) noexcept;
    auto operator=(HugeBuffer &&other) noexcept -> HugeBuffer &;
    HugeBuffer(const HugeBuffer &)                     = delete;
    auto operator=(const HugeBuffer &) -> HugeBuffer & = delete;

    [[nodiscard]] auto data() const noexcept -> void * { return data_; }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
    [[nodiscard]] auto mode() const noexcept -> PageMode { return mode_; }
    // bytes of this mapping currently backed by huge pages (reads smaps)
    [[nodiscard]] auto huge_bytes() const -> std::size_t;

  private:
    void release() noexcept;

    void       *data_ = nullptr;
    std::size_t size_ = 0;
    PageMode    mode_ = PageMode::Small;
};

} // namespace lob::mem
