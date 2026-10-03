#pragma once
// Hardware performance counters around a region of interest.
//
// PerfGroup: in-process counting with perf_event_open on kernel-generic events
//   (cycles, instructions, branch / L1d / dTLB / LL misses). The kernel maps
//   them to the right raw encodings on Intel and AMD, so the same binary works
//   on the VM and on bare metal. Events the PMU does not support (e.g. LL in
//   this VM) are skipped and reported. User space only (exclude_kernel).
//
// PerfControl: lets an *external* `perf stat|record -D -1 --control fd:C,A`
//   count only the region of interest. If LOB_PERF_CTL_FD / LOB_PERF_ACK_FD are
//   set (scripts/perf.sh does this), enable()/disable() send the commands and
//   wait for perf's ack; otherwise they are no-ops.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lob::harness {

struct CounterDef {
    std::string_view name;
    std::uint32_t    type;   // PERF_TYPE_*
    std::uint64_t    config; // PERF_COUNT_* / cache encoding
};

// cycles, instructions, branches, branch-misses, L1-dcache-load-misses,
// dTLB-load-misses, LLC-load-misses (7 events: 2 fixed + 5 general counters)
auto default_counters() -> std::span<const CounterDef>;

class PerfGroup {
  public:
    explicit PerfGroup(std::span<const CounterDef> defs);
    ~PerfGroup();
    PerfGroup(const PerfGroup &)                     = delete;
    auto operator=(const PerfGroup &) -> PerfGroup & = delete;

    [[nodiscard]] auto ok() const noexcept -> bool { return leader_ >= 0; }
    [[nodiscard]] auto error() const -> const std::string & { return error_; }
    [[nodiscard]] auto names() const -> const std::vector<std::string> & {
        return names_;
    }
    [[nodiscard]] auto skipped() const -> const std::vector<std::string> & {
        return skipped_;
    }

    void start() noexcept;                 // reset + enable
    void stop() noexcept;                  // disable

    struct Reading {
        std::vector<std::uint64_t> values; // raw counts, same order as names()
        double                     running_ratio =
            0; // time_running / time_enabled; < 1 => multiplexed
    };
    [[nodiscard]] auto read() const -> Reading;

  private:
    int                      leader_ = -1;
    std::vector<int>         fds_;
    std::vector<std::string> names_;
    std::vector<std::string> skipped_;
    std::string              error_;
};

class PerfControl {
  public:
    PerfControl();
    void               enable() noexcept { send("enable\n"); }
    void               disable() noexcept { send("disable\n"); }
    [[nodiscard]] auto active() const noexcept -> bool { return ctl_ >= 0; }

  private:
    void send(std::string_view cmd) noexcept;
    int  ctl_ = -1;
    int  ack_ = -1;
};

} // namespace lob::harness
