#!/usr/bin/env bash
# Ablation of v4 (one cost removed per variant), interleaved in one process.
# Generates ablate.hpp from include/lob/v4/book.hpp (two compile-time switches:
# kCheckDup, kTick), builds the driver against build/release/liblob_core.a and
# runs it on the given workloads. Not part of the CMake build on purpose.
#   tools/ablate_v4/build.sh data/itch12302019_AAPL.ops [more.ops ...]
source "$(dirname "${BASH_SOURCE[0]}")/../../scripts/common.sh"
here="$LOB_ROOT/tools/ablate_v4"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
python3 - "$LOB_ROOT/include/lob/v4/book.hpp" "$out/ablate.hpp" <<'PY'
import sys
s = open(sys.argv[1]).read()
for a, b in [
    ("namespace lob::v4 {", "namespace lob::abl {"),
    ("template <EventSink Sink, unsigned WindowBits = 16>\nclass BookT {",
     "template <EventSink Sink, unsigned WindowBits = 16, bool kCheckDup = true, Price kTick = 0>\nclass BookT {"),
    ("    if (index_.contains(id)) return reject(id, OpType::Push, RejectReason::DuplicateId);\n    add(id, side, price, qty);",
     "    if constexpr (kCheckDup)\n      if (index_.contains(id)) return reject(id, OpType::Push, RejectReason::DuplicateId);\n    add(id, side, price, qty);"),
    ("    const auto ut = static_cast<std::uint64_t>(tick_);",
     "    const auto ut = kTick != 0 ? static_cast<std::uint64_t>(kTick) : static_cast<std::uint64_t>(tick_);"),
    ("}  // namespace lob::v4", "}  // namespace lob::abl"),
]:
    assert s.count(a) == 1, a[:40]
    s = s.replace(a, b)
open(sys.argv[2], "w").write(s)
PY
lob_build release >/dev/null
clang++ -std=c++26 -stdlib=libc++ -O3 -march=native -g -DNDEBUG -I "$LOB_ROOT/include" -I "$out" \
    -isystem "$VCPKG_ROOT/installed/x64-linux-clang-libcxx/include" \
    "$here/driver.cxx" "$LOB_ROOT/build/release/liblob_core.a" -lc++abi -o "$out/driver"
for w in "$@"; do taskset -c "$LOB_CPU" "$out/driver" "$w" "${LOB_ROUNDS:-9}"; done
