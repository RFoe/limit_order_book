#!/usr/bin/env bash
# Sweep the index hash of v4 (default boost hash vs LocalityHash<K> for several
# K), interleaved in one process. Generates sweep.hpp from include/lob/v4/book.hpp
# with the index hash as a template parameter. Not part of the CMake build.
#   tools/sweep_v5_hash/build.sh data/itch12302019_AAPL.ops [more.ops ...]
source "$(dirname "${BASH_SOURCE[0]}")/../../scripts/common.sh"
here="$LOB_ROOT/tools/sweep_v5_hash"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
python3 - "$LOB_ROOT/include/lob/v4/book.hpp" "$out/sweep.hpp" <<'PY'
import sys
s = open(sys.argv[1]).read()
for a, b in [
    ("namespace lob::v4 {", "namespace lob::swp {"),
    ("template <EventSink Sink, unsigned WindowBits = 16>\nclass BookT {",
     "template <EventSink Sink, unsigned WindowBits = 16, class IndexHash = boost::hash<OrderId>, std::size_t kReserve = 0>\nclass BookT {"),
    ("    if (tick_ < 1) throw std::invalid_argument(\"v4: tick must be >= 1\");",
     "    if (tick_ < 1) throw std::invalid_argument(\"v4: tick must be >= 1\");\n    if constexpr (kReserve != 0) index_.reserve(kReserve);"),
    ("  using Index = boost::unordered_flat_map<OrderId, Loc>;",
     "  using Index = boost::unordered_flat_map<OrderId, Loc, IndexHash>;"),
    ("}  // namespace lob::v4", "}  // namespace lob::swp"),
]:
    assert s.count(a) == 1, a[:40]
    s = s.replace(a, b)
open(sys.argv[2], "w").write(s)
PY
lob_build release >/dev/null
clang++ -std=c++26 -stdlib=libc++ -O3 -march=native -g -DNDEBUG -I "$LOB_ROOT/include" -I "$out" \
    -isystem "$VCPKG_ROOT/installed/x64-linux-clang-libcxx/include" \
    "$here/driver.cxx" "$LOB_ROOT/build/release/liblob_core.a" -lc++abi -o "$out/driver"
for w in "$@"; do taskset -c "$LOB_CPU" "$out/driver" "$w" "${LOB_ROUNDS:-7}"; done
