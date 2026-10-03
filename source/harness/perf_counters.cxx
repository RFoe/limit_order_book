#include <lob/harness/perf_counters.hpp>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <format>

namespace lob::harness {

namespace {

constexpr auto
cache_event(std::uint64_t cache, std::uint64_t op, std::uint64_t result)
    -> std::uint64_t {
    return cache | (op << 8) | (result << 16);
}

constexpr std::array<CounterDef, 7> kDefault = {
    {
     {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
     {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
     {"branches", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
     {"branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
     {"L1d-load-misses", PERF_TYPE_HW_CACHE,
         cache_event(
             PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ,
             PERF_COUNT_HW_CACHE_RESULT_MISS)},
     {"dTLB-load-misses", PERF_TYPE_HW_CACHE,
         cache_event(
             PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ,
             PERF_COUNT_HW_CACHE_RESULT_MISS)},
     {"LL-load-misses", PERF_TYPE_HW_CACHE,
         cache_event(
             PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ,
             PERF_COUNT_HW_CACHE_RESULT_MISS)},
     }
};

auto perf_event_open(perf_event_attr *attr, int group_fd) -> int {
    return static_cast<int>(syscall(
        SYS_perf_event_open, attr, 0 /*this thread*/, -1 /*any cpu*/, group_fd,
        0));
}

auto env_fd(const char *name) -> int {
    const char *v = std::getenv(name);
    if (v == nullptr) return -1;
    int         fd  = -1;
    const auto *end = v + std::strlen(v);
    if (std::from_chars(v, end, fd).ptr != end) return -1;
    return fd;
}

} // namespace

auto default_counters() -> std::span<const CounterDef> { return kDefault; }

PerfGroup::PerfGroup(std::span<const CounterDef> defs) {
    for (const auto &d : defs) {
        perf_event_attr attr{};
        attr.size   = sizeof attr;
        attr.type   = d.type;
        attr.config = d.config;
        attr.disabled =
            leader_ < 0 ? 1 : 0; // the group is controlled through the leader
        attr.exclude_kernel = 1;
        attr.exclude_hv     = 1;
        attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                           PERF_FORMAT_TOTAL_TIME_RUNNING;
        const int fd     = perf_event_open(&attr, leader_);
        if (fd < 0) {
            const int err = errno;
            if (leader_ < 0 && (err == EACCES || err == EPERM)) {
                error_ = std::format(
                    "perf_event_open: {} (check "
                    "/proc/sys/kernel/perf_event_paranoid)",
                    std::strerror(err));
                return;
            }
            skipped_.push_back(
                std::format("{} ({})", d.name, std::strerror(err)));
            continue;
        }
        if (leader_ < 0) leader_ = fd;
        fds_.push_back(fd);
        names_.emplace_back(d.name);
    }
    if (leader_ < 0 && error_.empty())
        error_ = "no hardware event could be opened (no PMU?)";
}

PerfGroup::~PerfGroup() {
    for (int fd : fds_) close(fd);
}

void PerfGroup::start() noexcept {
    if (leader_ < 0) return;
    ioctl(leader_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(leader_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

void PerfGroup::stop() noexcept {
    if (leader_ < 0) return;
    ioctl(leader_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
}

auto PerfGroup::read() const -> Reading {
    Reading r;
    if (leader_ < 0) return r;
    // layout: nr, time_enabled, time_running, value[nr]
    std::vector<std::uint64_t> buf(3 + fds_.size());
    const auto                 bytes =
        ::read(leader_, buf.data(), buf.size() * sizeof(std::uint64_t));
    if (bytes < static_cast<ssize_t>(3 * sizeof(std::uint64_t))) return r;
    const std::uint64_t n = buf[0];
    r.running_ratio =
        buf[1] == 0 ? 0.0
                    : static_cast<double>(buf[2]) / static_cast<double>(buf[1]);
    r.values.assign(
        buf.begin() + 3, buf.begin() + 3 + static_cast<std::ptrdiff_t>(n));
    return r;
}

PerfControl::PerfControl()
    : ctl_(env_fd("LOB_PERF_CTL_FD")), ack_(env_fd("LOB_PERF_ACK_FD")) {}

void PerfControl::send(std::string_view cmd) noexcept {
    if (ctl_ < 0) return;
    if (::write(ctl_, cmd.data(), cmd.size()) !=
        static_cast<ssize_t>(cmd.size()))
        return;
    if (ack_ < 0) return;
    char buf[8];
    (void)::read(
        ack_, buf,
        sizeof buf); // perf answers "ack\n" once the command took effect
}

} // namespace lob::harness
