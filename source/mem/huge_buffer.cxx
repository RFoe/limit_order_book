#include <lob/mem/huge_buffer.hpp>

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <utility>

#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23 // Linux >= 5.14
#endif

namespace lob::mem {

namespace {

auto round_up(std::size_t n, std::size_t a) -> std::size_t {
    return (n + a - 1) / a * a;
}

// touch every 4 KiB page if MADV_POPULATE_WRITE is unavailable
void prefault(void *p, std::size_t n) {
    if (madvise(p, n, MADV_POPULATE_WRITE) == 0) return;
    auto *bytes = static_cast<volatile unsigned char *>(p);
    for (std::size_t i = 0; i < n; i += 4096) bytes[i] = 0;
}

auto map_hugetlb(std::size_t n) -> void * {
    void *p = mmap(
        nullptr, n, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

// 2 MiB-aligned anonymous mapping (THP only backs aligned 2 MiB extents)
auto map_aligned(std::size_t n) -> void * {
    const std::size_t len = n + kHugePage;
    void *raw = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    const auto start   = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = round_up(start, kHugePage);
    if (aligned > start) munmap(raw, aligned - start);
    const auto end = aligned + n;
    if (start + len > end)
        munmap(reinterpret_cast<void *>(end), start + len - end);
    return reinterpret_cast<void *>(aligned);
}

auto requested_mode() -> PageMode {
    const char *env = std::getenv("LOB_HUGE");
    const std::string_view v = env != nullptr ? env : "auto";
    if (v == "off") return PageMode::Small;
    if (v == "thp") return PageMode::THP;
    return PageMode::HugeTLB; // auto | hugetlb
}

} // namespace

auto to_string(PageMode m) -> std::string_view {
    switch (m) {
    case PageMode::HugeTLB: return "hugetlb";
    case PageMode::THP: return "thp";
    case PageMode::Small: return "small";
    }
    return "?";
}

HugeBuffer::HugeBuffer(std::size_t bytes) {
    size_          = round_up(bytes, kHugePage);
    PageMode start = requested_mode();
    if (start == PageMode::HugeTLB) {
        if ((data_ = map_hugetlb(size_)) != nullptr) {
            mode_ = PageMode::HugeTLB;
            return;
        }
        start = PageMode::THP; // pool empty or not reserved
    }
    if ((data_ = map_aligned(size_)) == nullptr) throw std::bad_alloc();
    if (start == PageMode::THP && madvise(data_, size_, MADV_HUGEPAGE) == 0) {
        mode_ = PageMode::THP;
    } else {
        madvise(data_, size_, MADV_NOHUGEPAGE);
        mode_ = PageMode::Small;
    }
    prefault(data_, size_);
}

HugeBuffer::~HugeBuffer() { release(); }

HugeBuffer::HugeBuffer(HugeBuffer &&other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)), mode_(other.mode_) {}

auto HugeBuffer::operator=(HugeBuffer &&other) noexcept -> HugeBuffer & {
    if (this != &other) {
        release();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        mode_ = other.mode_;
    }
    return *this;
}

void HugeBuffer::release() noexcept {
    if (data_ != nullptr) munmap(data_, size_);
    data_ = nullptr;
}

auto HugeBuffer::huge_bytes() const -> std::size_t {
    if (data_ == nullptr) return 0;
    const auto     start = reinterpret_cast<std::uintptr_t>(data_);
    std::ifstream  smaps("/proc/self/smaps");
    bool           in_region = false;
    std::size_t    kb        = 0;
    for (std::string line; std::getline(smaps, line);) {
        // mapping header: "start-end perms ..."
        if (const auto dash = line.find('-');
            dash != std::string::npos && dash < 17 &&
            line.find(' ') > dash) {
            const auto lo = std::stoull(line.substr(0, dash), nullptr, 16);
            const auto hi = std::stoull(line.substr(dash + 1), nullptr, 16);
            in_region     = lo <= start && start < hi;
            continue;
        }
        if (!in_region) continue;
        std::istringstream fields(line);
        std::string        key;
        std::size_t        value = 0;
        fields >> key >> value;
        if (key == "AnonHugePages:" || key == "Private_Hugetlb:" ||
            key == "Shared_Hugetlb:")
            kb += value;
    }
    return kb * 1024;
}

} // namespace lob::mem
