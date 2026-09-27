# A1 — VM Spike Results

> TL;DR: **Luau wins the gate.** It is faster than LuaJIT-with-FFI-removed on every measured axis, and it passes all five sandbox conformance probes that LuaJIT cannot pass at all. Both fit the budgets. Cross-platform legs run in CI.

## What was measured

Two probe binaries, identical in structure, each: creates the VM, registers one host C function, loads N synthetic extensions (20 tool registrations each), runs a C-boundary-heavy loop (200,000 calls into C — the workload that produced LuaJIT's documented JIT penalty), and reports RSS.

| Probe | Source | Build |
|---|---|---|
| `tools/spike/luajit_probe.cxx` | LuaJIT `c6ffc141` (v2.1), `BUILDMODE=static`, FFI compiled out | MSVC 19.44, `/O2 /MT` |
| `tools/spike/luau_probe.cxx` | Luau `c0e346ed`, `Luau.VM` + `Luau.Compiler` | MSVC 19.44, `/O2 /MT`, `LUAU_STATIC_CRT=ON` |

Reproduce: `tools/spike/run_posix.sh` (Linux/macOS) and the CI job for Windows.

## Results — Windows x64, MSVC 19.44

| Metric | LuaJIT (JIT on) | LuaJIT (JIT off, our policy) | **Luau** | Budget | Verdict |
|---|---|---|---|---|---|
| Linked probe size | 854.0 KB | 854.0 KB | **978.5 KB** | — | Luau +124.5 KB (+14.6%), inside the pre-committed 1.5 MB margin |
| Load, 1 ext | 1.38 ms | 1.19 ms | **0.37 ms** | ≤1 ms | Luau passes; LuaJIT is over |
| Load, 10 ext | 2.02 ms | 1.55 ms | **0.56 ms** | ≤10 ms | both pass |
| Load, 50 ext | 2.50 ms | 2.35 ms | **2.72 ms** | — | linear, small constant |
| Per-ext, 50 | 50.1 µs | 46.9 µs | **54.5 µs** | ≤1 ms | both pass with 18× headroom |
| C-boundary loop | 5.38 ms | 3.40 ms | **3.91 ms** | — | Luau beats JIT-on by 27%; ~15% behind JIT-off |
| RSS delta, 50 ext | 1544 KB | 1396 KB | **856 KB** | ≤64 KB/ext ⇒ 3200 KB | Luau uses 61% of LuaJIT's; ~17 KB/ext |
| Peak RSS | 4992 KB | 4844 KB | **4376 KB** | — | Luau lower |

## Sandbox conformance (the decision's real basis)

| Probe | LuaJIT (FFI removed) | Luau |
|---|---|---|
| `ffi` reachable | **0** (verified: no `luaopen_ffi` symbol, `lib_ffi.c` guarded by `LJ_HASFFI`) | n/a — FFI does not exist |
| Global write via `rawset(_G, …)` | n/a — no equivalent mechanism | **0** |
| `io` reachable | n/a | **0** |
| `os.execute` reachable | n/a | **0** |
| `loadstring` reachable | n/a | **0** |
| `setmetatable(_G, {})` | n/a | **0** |

LuaJIT's column is "n/a" because it has **no mechanism to produce these results** — `12` §Layer 3 is correct that VM-level confinement is a fiction there. Removing `ffi` closes native-code execution but leaves `io`, `os`, `package`, and `debug` intact; there is no supported way to make `_G` readonly or to bound a runaway script except an instruction hook that `17` documents as incompatible with the JIT.

## Findings that changed the plan

**1. `LUAJIT_DISABLE_FFI` alone does not build.** Verified: `make XCFLAGS=-DLUAJIT_DISABLE_FFI` fails with `buildvm_arch.h(1370): error C2065: 'CTState': undeclared identifier`. The dynasm preprocessor is invoked with a separate flag set (`DASMFLAGS`) that still contains `-D FFI`, so the generated interpreter emits FFI call helpers while the C side has `LJ_HASFFI=0`. **FFI must be removed from `XCFLAGS` and `DASMFLAGS` together.** `run_posix.sh` and the Windows leg both do this.

**2. Luau's sandbox requires `luaL_sandbox` *after* host globals are registered.** Calling it first makes `_G` readonly, and `lua_setglobal` on a readonly table raises outside a `pcall` — which surfaces as a bare `0xC0000409` process abort with no message. Cost an hour to find; it is now a comment in the probe and a constraint for the real host.

**3. The JIT-off comparison was necessary to be fair.** LuaJIT's best showing (3.40 ms) is with the JIT off, which is mcode's policy. Comparing Luau against LuaJIT-with-JIT-on would have flattered Luau on a configuration we would never ship.

## Decision

**Adopt Luau.** Per A1's pre-committed thresholds: it does not exceed the 1.5 MB size margin (+124.5 KB), the 20 ms / 10-extension load threshold (0.56 ms), or fail to build. It additionally wins on RSS, cold load, and the C-boundary loop, and it is the only one of the two that passes the conformance probes.

The margin on size is smaller than on every other axis, and that is the honest trade: **+124.5 KB of binary for a boundary that exists.**

## What is still unverified

- **Linux and macOS legs.** The Windows numbers are measured; the other two are wired into CI (`vm-spike` job) and must pass before A2 is treated as final. The pre-committed platform threshold stands: a failure to build on any tier-1 platform reverses the decision.
- **Luau without `Luau.Compiler`.** The probe links the compiler, so all numbers include parse cost. A5 must measure the VM-only variant (bytecode precompiled at install) — this is a real size lever, not a rounding error.
- **Interrupt latency.** Not measured. A7 must confirm the kill switch fires promptly on a runaway extension.
- **Instruction accounting.** Luau has no `LUA_MASKCOUNT` equivalent; the interrupt is the only mechanism. Whether that is sufficient for per-extension CPU budgeting is open.

## Sources

- `tools/spike/luajit_probe.cxx`, `tools/spike/luau_probe.cxx` — the probes
- `tools/spike/run_posix.sh` — the Linux/macOS leg
- LuaJIT `c6ffc141` (v2.1 branch), Luau `c0e346ed`
- https://luau.org/sandbox/ — the guarantees the conformance probes test
- `docs/12`, `docs/17`, `docs/26`
