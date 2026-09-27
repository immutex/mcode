# Measurement Spine (B1–B4)

> TL;DR: Load and dispatch pass their budgets with large margins. **Per-extension memory does not** — one VM per extension costs ~320 KB, five times the 64 KB the plan assumed, and that assumption came from a shared-VM probe. The isolation is worth keeping; the budget was wrong and is revised here.

## How to reproduce

```powershell
cmake --build build/Release
./build/Release/tools/bench/mcode_bench.exe 50 8
```

`tools/bench` links the real `lua_host`, not a copy, so these numbers describe the shipped code. Output is `key=value` lines for CI gating. The harness registers the **full 28-name frozen surface** (`18`) with stubs and runs a synthetic extension that performs `tools_per_extension` registrations — a partial surface would understate load cost.

## B2 — extension load

Windows x64, MSVC 19.51, Release, `/MT`.

| Extensions | Total | Per extension | Budget |
|---|---|---|---|
| 1 | 0.417 ms | 417 µs | ≤1 ms first — **pass**, 2.4× headroom |
| 10 | 2.094 ms | 209 µs | ≤10 ms — **pass**, 4.8× headroom |
| 50 | 5.511 ms | 110 µs | ≤1 ms each — **pass**, 9× headroom |

**Cost is linear with a fixed first-VM term.** The marginal cost settles at ~110 µs; the first VM costs ~300 µs more because of page faults and allocator warmup. The plan asked whether load is O(1) or O(n) in extension count — it is **O(n) with a small constant**, and the `01` budget is rewritten to say that instead of "O(1) in extension count", which was never true.

Load cost is `luau_compile` + `luau_load` + the extension body. Compiling source is most of it; `A5`'s VM-only variant (precompiled bytecode) would cut it, at the cost of requiring signed bytecode.

## B3 — per-extension memory

**This is the finding that changed a budget.**

| Metric | Measured | Plan budget | Verdict |
|---|---|---|---|
| Allocated, mean per extension | **352,736 B (344 KB)** | ≤64 KB | **FAIL — 5.4× over** |
| Allocated, peak per extension | 385,456 B (376 KB) | — | — |
| **Bare VM** (no API surface, no extension code) | **320,016 B (313 KB)** | — | — |
| Extension's own cost, 8 tool registrations | ~33 KB | — | — |
| RSS delta, 50 extensions | 8,288 KB | — | ~166 KB/ext |

**Where it comes from.** `luaL_newstate` + `luaL_openlibs` + `luaL_sandbox` + the sandboxed thread is 313 KB — 91% of the total. The extension's own registrations are 33 KB. So the number is a **fixed per-VM cost**, not a function of what the extension does.

**Why the plan's 64 KB was wrong.** It came from A1's probe, which measured 856 KB RSS for 50 extensions — 17 KB each. But that probe used **one state with 50 sandboxed threads**, which is cheap. The refactor to one VM per extension (`25b0dea`) was made for a reason: a shared allocator cannot attribute bytes to an extension, and a single watchdog would fire on whichever extension happened to be running. **The 64 KB figure was measured against a design we deliberately abandoned.**

**Is the trade right?** Keep it. Against `01`'s 30 MB idle budget:

| Extensions | RSS | Share of budget |
|---|---|---|
| 10 | 1.7 MB | 5.6% |
| 20 | 3.3 MB | 11% |
| 50 | 8.3 MB | 28% |
| 100 | 16.6 MB | 55% |

Twenty extensions is the realistic default set, and it costs 3.3 MB. The alternative — one shared VM — saves 90% of that and gives up per-extension accounting and sound per-extension time budgets. Attribution is what `B3` exists to provide, and `12` lists memory exhaustion as the containment case the allocator ceiling is supposed to handle. Paying 313 KB per extension for it is the right side of that trade, but it is a real cost and it is now stated rather than assumed.

**Revised budget: 512 KB allocated per extension**, which leaves headroom over the measured 376 KB peak for an extension with real registrations. The 8 MB per-extension ceiling is unchanged — it bounds a runaway, and 8 MB is 25× the baseline.

**Reduction lever, not taken yet:** the stdlib is most of the 313 KB. Loading a reduced library set (`base`, `table`, `string`, `math` only — dropping `os`, `debug`, `utf8`, `bit32`, `buffer`, `vector`, `integer`, `coroutine`) would cut it substantially, and `luaL_sandbox` already removes most of `os`/`debug` from the script's view. Worth measuring before optimizing: it is a one-line change to test and a real ergonomics cost if wrong.

## B4 — hook dispatch

`dispatch(payload)` chaining N Lua handlers, 20,000 iterations, one extension.

| Handlers | µs per call | Budget |
|---|---|---|
| 0 (call overhead) | 0.095 | — |
| 1 | 0.157 | ≤500 µs per vetoable event — **pass, 3,000× headroom** |
| 10 | 0.704 | **pass, 710× headroom** |
| 50 | 3.631 | **pass, 138× headroom** |

**Cost is linear: ~0.072 µs per handler, 0.09 µs base.** Even 50 handlers on one event consume 3.6 µs — 0.07% of the 5 ms TTFT budget (`01`). The plan's ≤0.5 ms dispatch budget is not remotely at risk from handler count.

This settles a `18` open question with data: **synchronous hooks are affordable, and there is no reason to batch or coalesce delivery at our rates.** A single extension would need ~7,000 handlers on one event to reach the budget.

The budget stays at ≤0.5 ms because the risk it guards is not handler count — it is one handler doing something slow, which the interrupt catches (`18` §How a handler is protected).

## Budget changes this forces

| Doc | Was | Now |
|---|---|---|
| `01` extension load | "O(1) in extension count" | Linear, ~110 µs marginal, ≤1 ms first — measured (`28`) |
| `01`/`26` per-extension memory | ≤64 KB | **≤512 KB**, measured 344 KB mean / 376 KB peak (`28`) |
| `26` B2 acceptance | per-ext + aggregate load | Met |
| `26` B3 acceptance | `ext doctor` shows bytes per extension | Met — `bytes_allocated()` per host is exact |
| `26` B4 acceptance | dispatch budget | Met, 138× margin at 50 handlers |

## Open questions

- **Does the reduced stdlib set pay off?** Measure before adopting; it is the only lever with real headroom.
- **Is 100 extensions a case worth designing for?** At 16.6 MB it is over half the idle budget, and nothing in the plan suggests that scale.
- **Does the fixed cost shrink on Linux/macOS?** The CI legs will say. Allocator and page-size behaviour differ.
- **Cold start** (`01`'s ≤15 ms) is not measured here — the bench measures from `main` entry, and the process is up in 110 ms wall including 50 VM creations. A dedicated cold-start probe is still owed by `B1`.

## Sources

- `tools/bench/main.cxx` — the harness
- `tools/bench/CMakeLists.txt` — builds against the real `lua_host`
- `docs/27-a1-vm-spike.md` — the shared-VM probe whose 17 KB/ext figure the 64 KB budget came from
- `docs/17`, `docs/18`, `docs/12` §Layer 3 — the isolation model this measures
