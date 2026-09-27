#!/usr/bin/env bash
#
# A1 VM spike — POSIX leg (Linux and macOS).
#
# Builds Luau and LuaJIT-without-FFI from pinned commits, builds both probes,
# runs them, and prints key=value output identical in shape to the Windows leg
# so the three platforms are directly comparable.
#
# Usage: tools/spike/run_posix.sh [workdir]

set -euo pipefail

work="${1:-/tmp/mcode-spike}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

LUAU_COMMIT="c0e346edd89066b44dca174c9f54ce84c746a540"
LUAJIT_COMMIT="c6ffc141a8762b41703f9287d63d93622a13dd8f"

mkdir -p "$work"
cd "$work"

platform="$(uname -s)"
jobs="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )"

echo "platform=$platform"
c++ --version | head -1
cmake --version | head -1

# ---------------------------------------------------------------------------
echo "== Luau =="
if [ ! -d luau ]; then
    git clone -q https://github.com/luau-lang/luau.git
fi
git -C luau fetch -q --depth 1 origin "$LUAU_COMMIT" 2>/dev/null || true
git -C luau checkout -q "$LUAU_COMMIT"

cmake -S luau -B luau/build-vm -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLUAU_BUILD_CLI=OFF -DLUAU_BUILD_TESTS=OFF >/dev/null
cmake --build luau/build-vm --target Luau.VM Luau.Compiler Luau.Common Luau.Ast Luau.Bytecode >/dev/null
echo "luau_build=ok"

# ---------------------------------------------------------------------------
echo "== LuaJIT (FFI compiled out) =="
if [ ! -d LuaJIT ]; then
    git clone -q --branch v2.1 https://github.com/LuaJIT/LuaJIT.git
fi
git -C LuaJIT fetch -q --depth 1 origin "$LUAJIT_COMMIT" 2>/dev/null || true
git -C LuaJIT checkout -q "$LUAJIT_COMMIT"

# FFI must be removed from BOTH the C flags and the dynasm preprocessor flags.
# Setting only LUAJIT_DISABLE_FFI breaks the build: dynasm still emits FFI call
# helpers (buildvm_arch.h references CTState/CCallState) while the C side has
# LJ_HASFFI=0, which is a hard compile error.
make -C LuaJIT -j"$jobs" BUILDMODE=static \
    XCFLAGS="-DLUAJIT_DISABLE_FFI" \
    DASMFLAGS="-D ENDIAN_LE -D FPU -D P64" >/dev/null
echo "luajit_build=ok"

# ---------------------------------------------------------------------------
echo "== probes =="
c++ -O2 -std=c++20 -o luajit_probe \
    -I"LuaJIT/src" "$repo_root/tools/spike/luajit_probe.cxx" \
    "LuaJIT/src/libluajit.a" -lm -ldl

c++ -O2 -std=c++20 -o luau_probe \
    -I"luau/VM/include" -I"luau/Compiler/include" -I"luau/Common/include" \
    "$repo_root/tools/spike/luau_probe.cxx" \
    luau/build-vm/libLuau.VM.a luau/build-vm/libLuau.Compiler.a \
    luau/build-vm/libLuau.Ast.a luau/build-vm/libLuau.Common.a \
    luau/build-vm/libLuau.Bytecode.a -lm

echo "probes_build=ok"

# ---------------------------------------------------------------------------
echo
echo "== linked size =="
printf 'luajit_probe_bytes=%s\n' "$(stat -f %z luajit_probe 2>/dev/null || stat -c %s luajit_probe)"
printf 'luau_probe_bytes=%s\n' "$(stat -f %z luau_probe 2>/dev/null || stat -c %s luau_probe)"

echo
echo "== luajit (jit off — mcode policy) =="
./luajit_probe 50 200000 jitoff

echo
echo "== luau =="
./luau_probe 50 200000
