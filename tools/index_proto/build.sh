#!/usr/bin/env bash
# Order-index prototypes replayed in isolation (see proto.cxx). Not part of the
# CMake build.
#   tools/index_proto/build.sh data/itch12302019_AAPL.ops [more.ops ...]
# LOB_ROUNDS (default 9) rounds per workload, candidates interleaved.
source "$(dirname "${BASH_SOURCE[0]}")/../../scripts/common.sh"
here="$LOB_ROOT/tools/index_proto"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
lob_build release >/dev/null
clang++ -std=c++26 -stdlib=libc++ -O3 -march=native -g -DNDEBUG -Wall -Wextra -I "$LOB_ROOT/include" \
    -isystem "$VCPKG_ROOT/installed/x64-linux-clang-libcxx/include" \
    "$here/proto.cxx" "$LOB_ROOT/build/release/liblob_core.a" -lc++abi -o "$out/proto"
for w in "$@"; do
    taskset -c "$LOB_CPU" "$out/proto" "$w" "${LOB_ROUNDS:-9}"
done
