# Extension Runtime

> TL;DR: **Luau** is the extension VM — chosen over LuaJIT because it is the only in-process runtime with a designed, adversarially-tested capability boundary, and because it measured faster and smaller on every axis that matters (`27`). JIT is not involved; the VM is created lazily; `ffi` does not exist.

## The decision

`27` records the spike. Summary, Windows x64 / MSVC 19.44, 50 extensions:

| Axis | LuaJIT (FFI removed, JIT off) | **Luau** | Note |
|---|---|---|---|
| Linked probe | 854.0 KB | 978.5 KB | +124.5 KB — the honest price of the boundary |
| Load, 1 extension | 1.19 ms | **0.37 ms** | budget ≤1 ms |
| Per-extension, 50 | 46.9 µs | 54.5 µs | both ~18× under budget |
| C-boundary loop | 3.40 ms | 3.91 ms | ~15% behind |
| RSS, 50 extensions | 1396 KB | **856 KB** | ~17 KB/ext, budget ≤64 KB |
| Sandbox conformance | **cannot pass** | **passes all 5** | the decision's real basis |

LuaJIT cannot pass the conformance probes because it has no mechanism to produce them: no readonly `_G`, no way to remove `io`/`os`/`package`/`debug`, and no supported way to bound a runaway script (`LUA_MASKCOUNT` does not fire under JIT, per `17`'s superseded evaluation below). Removing `ffi` closes native-code execution but leaves the rest open.

**The trade is +124.5 KB of binary for a boundary that exists.**

## Why Luau

The requirement is that extensions must not be able to do harm. That forfeits LuaJIT's two advantages before the comparison starts: `ffi` must go (it is a one-line native-code escape), and the JIT is off by default anyway (`17` below, measured 2.1× penalty on C-boundary-heavy glue — the workload a coding-agent extension layer *is*).

What remains is size and Lua familiarity. Neither outweighs a boundary that is designed, fuzzed, and exercised at Roblox scale.

**RAM efficiency points the same way.** The only formally stronger boundary is a separate process — a second address space, an IPC channel, and a supervisor, which is the VS Code extension-host model and the reason VS Code is heavy. In-process Luau is the lighter of the two viable answers, and `27` confirms it is also lighter than LuaJIT in RSS.

## What the boundary actually guarantees

From `luau.org/sandbox` (verbatim scope):

| Property | Mechanism |
|---|---|
| No filesystem, no process execution, no native module loading | `io.` and `package.` removed entirely; `os.` reduced to `clock`/`date`/`difftime`/`time` |
| No reflection into the host | `debug.` reduced to `traceback`/`info`; `dofile`/`loadfile` removed |
| No bytecode | `loadstring` rejects bytecode, `string.dump` and `load` are gone |
| Globals cannot be monkey-patched | `_G`, every library table, and the string metatable are readonly via a VM feature unreachable from scripts — assignments, `rawset`, and `setmetatable` all fail |
| No finalizer reentrancy or use-after-finalize | `__gc` does not exist; host-only tag-based destructors (`lua_newuserdatadtor`) run before the block is freed |
| Runaway scripts terminate | A host interrupt that "any Luau code is guaranteed to call … eventually (in practice … at any function call or at any loop iteration)" |
| Memory ceiling | Configurable from the host |

**The limit, stated plainly:** upstream says "since the entire stack is implemented in C++, the sandboxing isn't formally proven." That is the honest position. This is a designed, in-process capability boundary that reduces blast radius — **not an OS sandbox and not a guarantee.** `12` owns the full statement; `A4` keeps the process tier reachable as a later upgrade rather than a redesign. Never describe this as a sandbox in user-facing text.

## Embedding shape

- **Two build variants.** `Luau.VM` alone (bytecode in, no parser) or `Luau.VM` + `Luau.Compiler` (source in). `A5` measures both; the VM-only variant is a real size lever, and precompiled bytecode must be signed because the VM trusts its compiler.
- **CMake, no external dependencies** beyond the STL/CRT. MSVC 2017+, gcc-7+, clang-7+. `LUAU_STATIC_CRT=ON` matches our `/MT`.
- **Ordering constraint, learned the hard way:** register host globals **before** `luaL_sandbox`. Sandboxing makes `_G` readonly, and `lua_setglobal` on a readonly table raises outside a `pcall` — which surfaces as a bare process abort with no message.
- **Sealing is a one-way door, and that is by design.** Luau checks `readonly` on *every* C API write path, so once `luaL_sandbox` runs the host cannot add an API entry either. `18` freezes the surface at 22 entries partly for forward-compatibility and partly because this is physically required: **the whole API surface must exist before the first line of extension code runs.** `lua_host` enforces it — registration after sealing returns an error, and the smoke test asserts it.
- **`lua_tolstring` does not convert booleans.** It converts strings and numbers only, so a host helper that reads a result via `lua_tolstring` silently sees `nil` for `true`/`false` and reports a placeholder. Route non-string results through the VM's own `tostring`.
- Per-extension isolation: `lua_newthread` + `luaL_sandboxthread` per extension; the thread value sits on the parent stack and must be popped after use.
- **Memory ceiling and accounting** via a custom allocator, which is also how `mcode ext doctor` attributes bytes per extension (`B3`).
- **Every host→Lua entry point goes through `lua_pcall`.** An unprotected error aborts the process.
- **Every exposed C++ function must be bounded in time.** The interrupt cannot preempt inside a single long-running host call, so an unbounded host function defeats the kill switch.

**Threading:** one VM per OS thread; a `lua_State` is not thread-safe. Either drive all Lua from the loop thread, or give each worker its own state with no shared objects.

## Dialect: what extension authors get

Luau is **Lua 5.1-based** with 5.2/5.3 features backported and its own additions. This is a different dialect from LuaJIT's, so `17`'s superseded "LuaJIT 3.0 syntax backports" section does **not** apply.

| Available | Missing (vs Lua 5.4) |
|---|---|
| Gradual typing with a real inference engine; `luau-analyze` lints and type-checks | `//` floor division, `utf8` library, `<close>` / to-be-closed variables |
| `continue`, compound assignment (`+=`, `..=`), string interpolation | 64-bit integer subtype (numbers are doubles, exact to 2^53) |
| `table.clone`, `table.freeze`, `string.split`, vector library | `io`, `package`, most of `os` and `debug` — by design |
| `require` with a host-controlled resolver | `loadstring` on bytecode, `string.dump` |

**Practical rule for the extension guide: Lua 5.1 + Luau idioms work; Lua 5.4-era copy-paste does not.** `C5` ships a `.d.luau` definition file so `luau-analyze` type-checks extension code.

## Memory

| Item | Value |
|---|---|
| VM + compiler, linked | 978.5 KB (`27`, probe binary including our host code) |
| RSS, 50 extensions | 856 KB delta ⇒ ~17 KB per extension |
| Peak RSS | 4376 KB |

Allocator routing matters more than for LuaJIT: Luau's memory is ours to count and cap, which is what makes the per-extension ceiling enforceable rather than advisory.

## Traps

- **Calling it a sandbox.** It is a capability boundary. `12` owns the wording; user-facing text never says "sandbox."
- **`luaL_sandbox` before registering host globals.** Process abort, no message.
- **Unbounded host functions.** The interrupt cannot preempt inside one, so the kill switch is only as good as the shortest bounded call.
- **Forgetting `luaL_sandboxthread` per extension thread.** Isolation is per-thread and opt-in.
- **Expecting Lua 5.4 semantics.** No integers, no `//`, no `utf8`, no `<close>`.
- **Bytecode from an untrusted source.** The VM trusts its compiler; signing is mandatory if bytecode ever ships.
- **Treating `debug.traceback`/`info` as harmless.** They are retained for diagnostics; confirm they leak nothing about host internals (`A7`).
- **Leaving the compiler linked when bytecode is precompiled.** Wasted size, and it re-admits a parser that untrusted source could reach.

## Open questions

- **Is the interrupt enough for CPU budgeting?** Luau has no `LUA_MASKCOUNT` equivalent. Measure interrupt latency; if it is coarse, a host-side wall-clock kill is the backstop.
- **Per-extension thread vs one shared sandboxed thread?** safeenv may make a shared thread sufficient, which would cut load cost. Measure in `B2`.
- **VM-only (precompiled bytecode) vs VM + compiler?** `A5` measures the delta. Bytecode needs signing and makes extensions unreviewable as source.
- **Does `debug.info` leak host state?** `A7` probes it.

## Superseded: the LuaJIT evaluation (2026-09, kept for the record)

**This section is DISPROVED as a decision.** LuaJIT was chosen on FFI, JIT throughput, and size. The "extensions must not be able to do harm" requirement forfeits the first two, and `27` measured the third. Kept so the reasoning is not re-proposed.

- **Why it was chosen:** ~0.5 MB VM, native MSVC support, 2–15× a stock Lua interpreter, and an FFI that made C-level extension work practical.
- **Version pinning:** LuaJIT uses rolling releases — no tags. Pin a `v2.1` **commit**, never a version string; `2.1.0-beta3` is a 2017 snapshot. Conan Center is stale at that label.
- **JIT policy, measured:** a DHCP loop with C callbacks ran 1.274 s with JIT on versus 0.603 s with `jit.off()` — **2.1× faster without the JIT**, because C-side metatable and userdata operations abort traces and flush the code cache. Instruction hooks (`LUA_MASKCOUNT`) do not fire under JIT without an unsupported build flag, so budget enforcement and JIT are mutually exclusive. Our `27` measurements reproduce this: the JIT-on boundary loop (5.38 ms) is slower than JIT-off (3.40 ms).
- **FFI removal, verified:** `LUAJIT_DISABLE_FFI` sets `LJ_HASFFI 0`, which removes `luaopen_ffi` from `lib_init.c` and compiles out the bytecode cdata-literal reader (`lj_bcread.c:239`), rejecting any `BCDUMP_F_FFI` chunk (`:405`). This **corrects `12`'s claim** that a bytecode cdata literal re-initializes FFI — true for runtime hiding (`ffi = nil`), false for compile-time removal.
- **FFI removal does not build alone:** dynasm is invoked with a separate flag set (`DASMFLAGS`) that still contains `-D FFI`, so `buildvm_arch.h` references `CTState`/`CCallState` while the C side has `LJ_HASFFI=0` — a hard compile error. FFI must come out of `XCFLAGS` **and** `DASMFLAGS` together.
- **GC64** is default on 64-bit ports at +8–11% RSS `[VENDOR]`.
- **Bus factor 1** (Mike Pall); OpenResty's `luajit2` is a synchronized downstream.

## Sources

- `docs/27-a1-vm-spike.md` — the measurements, probes, and reproduction
- https://luau.org/sandbox/ — library removals, bytecode removal, readonly globals, `__gc` removal, interrupt mechanism, memory limits, upstream caveat
- https://github.com/luau-lang/luau — CMake build, no external dependencies, MSVC 2017+/gcc-7+/clang-7+, `luau_compile`/`luau_load` split, `luaL_sandbox`/`luaL_sandboxthread`/`lua_newuserdatadtor`, MIT plus attribution request
- https://luajit.org/faq.html — process-level sandboxing stance, bytecode danger (superseded section)
- https://marek.vavrusa.com/embedding-luajit/ — the `jit.off()` 2.1× measurement (superseded section)
- https://blog.openresty.com/en/luajit-gc64-mode/ — GC64 cost (superseded section)
- https://luajit.org/status.html — rolling releases, commit pinning (superseded section)
- https://github.com/LuaJIT/LuaJIT/issues/779 — `LUA_MASKCOUNT` under JIT (superseded section)
- https://www.corsix.org/content/malicious-luajit-bytecode — the cdata-literal escape (superseded section)
