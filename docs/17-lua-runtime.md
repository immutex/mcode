# Lua Runtime

> TL;DR: Embed LuaJIT 2.1 (rolling release, commit-pinned) as the extension VM with **JIT off by default** — measured 2.1× faster for C-boundary-heavy CLI workloads — one `lua_State` per thread, custom allocator routed to mimalloc, and no pretence that the VM is a security boundary.

## Why LuaJIT

The extension layer is the product differentiator: a native binary with a scripting ecosystem nobody else in this space has. LuaJIT is the only runtime that combines (a) a ~0.5 MB VM, (b) native MSVC support, (c) 2–15× the throughput of a stock Lua interpreter, and (d) an FFI that makes C-level extension work practical.

| Candidate | Size added | MSVC | Perf vs LuaJIT | Sandbox | Verdict |
|---|---|---|---|---|---|
| **LuaJIT 2.1** | ~0.4–0.6 MB lib | native (`msvcbuild.bat`), full C++ exception interop | baseline | none at VM level (`12`) | **Chosen** |
| Lua 5.4.9 / 5.5.1 | ~0.2–0.3 MB | first-class | 2–15× slower interpreted | moderate | Rejected: no FFI, slower, no reason to give up perf |
| Luau 0.740 | larger (VM + compiler + analysis) | supported (VS2017+) | ≈ LuaJIT interpreter; optional non-tracing JIT | **best** — designed sandbox, no bytecode loading, no `__gc` | Rejected for now; **the fallback if untrusted plugin distribution becomes a goal** |
| QuickJS-ng | ~370 KB claimed `[VENDOR]` | historically rough (MSVC issues) | ≈ PUC Lua interpreter | good | Rejected: MSVC friction, slower, wrong language for the ecosystem |
| Wasm3 / WAMR | ~64 KB (wasm3) | yes | interpreter tier | strongest | Rejected: extension authors must target Wasm; kills the ergonomics story |
| V8 | tens of MB | yes | faster aggregate | strong | Rejected: violates the entire size thesis |

Evidence for the scripting-in-a-coding-agent bet: **Maki** (Rust, MIT) embeds **Luau** with an API that deliberately mirrors Neovim (`vim.fs`, `vim.uv`, `vim.keymap`), `plugin.toml` capability manifests, and `/reload` hot-swap. Luau is *not* LuaJIT — different dialect, different JIT (optional, non-tracing), no FFI — so it is an architectural precedent, not a language one. **imp** (Rust) embeds Lua via `mlua` as its stable extension path with `before_tool_call` hooks that can block. Both are small projects. No mainstream harness ([CC], Codex, opencode, Pi, Goose) embeds a scripting language — they are all JS/TS-on-a-runtime where the runtime *is* the extension language. mcode would be the first serious harness with a native core plus a scripting ecosystem.

## Version pinning (non-obvious, get this right)

LuaJIT uses **rolling releases**: no tarballs, no binaries, **no git tags**. The version is `2.1.<unix-timestamp-of-last-commit>`, shown by `luajit -v`.

Consequences:
- Pin a **commit hash** of the `v2.1` branch, never a version string. Record the timestamp version alongside it.
- `v2.1` is the "Production" branch with no breaking changes. The old `master` is pinned to v2.0.
- The `2.1.0-beta3` label is stale (2017) and still appears in benchmarks and packaging. Do not treat it as current.
- **Conan Center is stale** at `luajit/2.1.0-beta3`; vcpkg tracks the rolling branch. Either vendor the source with a thin CMake shim around `msvcbuild.bat`/`make`, or carry a private Conan recipe that pins the commit.
- OpenResty's `luajit2` is a synchronized downstream (not a hard fork) with extra APIs (`table.clone`, `lua_resetthread`) and tuned JIT defaults. Use upstream unless those specific APIs are needed.

Bus factor is 1 (Mike Pall). Mitigation: the diff surface per commit is tiny, the branch is stable by policy, and a downstream fork exists. Vendor the source tree so builds never depend on upstream availability.

## Platform support

| Platform | Status |
|---|---|
| Windows x64 / x86 / **ARM64** | Supported. `src/msvcbuild.bat` for MSVC; x64→ARM64 cross via `vcvarsall.bat x64_arm64` |
| Linux x64 / ARM64 | Supported |
| macOS 10.4+ / Apple Silicon | Supported; needs `MACOSX_DEPLOYMENT_TARGET` |
| iOS | JIT disabled by platform policy — interpreter only |
| RISC-V | No JIT in 2.1; "(TBA)" |

Two Windows-specific facts that shape the build:

- **Lua errors are implemented with SEH** on Windows x64. Every `lua_pcall` error path is a Windows exception. Catching Lua errors with C++ `catch(...)` requires `/EHa` (which also catches access violations — an unwanted side effect). Prefer the `lua_pcall` boundary and do not throw C++ exceptions through Lua frames.
- **JIT machine code must be allocated within ±1 GB of the VM code** (branch range). Failures surface as "failed to allocate mcode memory" plus trace-flush churn. Default `sizemcode=64` KB per area, `maxmcode=2048` KB total.

**GC64 is default on all 64-bit ports** and mandatory on ARM64. It lifts the old ~2 GB low-address ceiling to 128 TB, at a measured cost of **+8–11% RSS with no CPU change** (OpenResty, `[VENDOR]`). Bytecode format differs between GC64 and non-GC64 and between 32/64-bit — relevant only if we ever ship precompiled bytecode across platforms. We ship x64/arm64 GC64 only.

## Dialect: what extension authors get

Baseline is **Lua 5.1 plus backports**. Set expectations explicitly in the extension docs, because the gaps bite:

| Available | Missing |
|---|---|
| `goto`/labels, `\x`/`\z` escapes, `load` with env (5.2) | 64-bit **integer subtype** — all numbers are doubles (exact to 2^53) |
| `\u{XX}` escapes, `table.move`, `coroutine.isyieldable` (5.3) | `//` floor division |
| LuaJIT 3.0 syntax **backported into 2.1**: `&& \|\| !`, ternary `?:`, safe-nav `?.`, `??`, compound assignment (`+=`, `..=`), `continue`, `const`, digit separators | 5.3 bitwise operator syntax (use the `bit.*` library, 32-bit ops) |
| `bit.*` (Lua BitOp), FFI for 64-bit cdata arithmetic | `utf8` library |
| Fully resumable VM (yield across `pcall`, iterators, metamethods) | `<close>` / to-be-closed variables (5.4) |
| `table.new`, `table.clear`, 64-bit `io.*` offsets, `xpcall` with args | `math.type`, `string.pack` 5.3 semantics, `_ENV` |

Practical rule for the extension guide: **Lua 5.1 + LuaJIT idioms work; Lua 5.4-era copy-paste does not.** Ship a short porting note.

## JIT policy: off by default

This is the counterintuitive decision, and it is evidence-backed.

LuaJIT's warmup is genuinely cheap — `hotloop=56` iterations before tracing, and compilation is "microsecond to millisecond range" per the official docs. So warmup is not the problem.

The problem is **trace-flush churn from C-boundary-heavy code**. A measured embedding (DHCP loop with C callbacks) ran **1.274 s with JIT on versus 0.603 s with `jit.off()` — 2.1× faster without the JIT**, because C-side metatable and userdata operations caused constant trace aborts and full machine-code cache flushes. Metatable operations from C in hot paths are a known trace-abort source.

A coding-agent extension layer is exactly that workload: small Lua glue scripts orchestrating calls into C (filesystem, process, HTTP, tools), not long numeric loops.

**Policy:**

| Path | JIT | Rationale |
|---|---|---|
| One-shot CLI commands, config eval, single extension invocation | **off** | Measured faster; process may live <10 s; no warmup to amortize |
| Long-lived loops — watch mode, server mode, bulk transforms | **on** | Where tracing pays; expose as a per-extension manifest flag |
| Instruction-budget-enforced paths | **off (forced)** | `LUA_MASKCOUNT` hooks do not fire in JIT-compiled code (below) |

The interpreter alone is already 2–15× a stock Lua 5.4 interpreter (n-body 14.9×, spectral-norm >19×, binarytrees 2.8×), so the performance story does not depend on the JIT.

**Interlock with resource limits:** `LUA_MASKCOUNT` hooks **do not fire inside JIT-compiled traces** unless LuaJIT is built with `-DLUAJIT_ENABLE_CHECKHOOK`, which is explicitly unsupported and "may be quite expensive in tight loops." So any extension running under an instruction budget must run interpreted. `jit.off()` default and budget enforcement are therefore the same decision, not two.

## Embedding shape

- `luaL_newstate()` + `luaL_openlibs(L)`, then **remove** what extensions must not have (`12`, `18`).
- **Custom allocator** via `lua_newstate(lua_Alloc, void*)` routed to mimalloc, with an optional hard ceiling for budget enforcement. Note LuaJIT otherwise uses its own bundled dlmalloc-derived allocator; `LUAJIT_USE_SYSMALLOC` switches it to the system allocator, and mimalloc override does not otherwise touch Lua's heap.
- Register host functions with `lua_pushcclosure` / `luaL_setfuncs` / `luaL_newlib`.
- **userdata for owned objects, lightuserdata for borrows.** Light userdata is a raw pointer value and is *not* GC-managed — use it only for non-owned handles.
- Store callbacks with `luaL_ref(L, LUA_REGISTRYINDEX)`, not repeated `lua_getglobal`.
- **Every host→Lua entry point is wrapped in `lua_pcall`.** An unprotected OOM aborts the process with `PANIC: unprotected error in call to Lua API`.
- Optional: install one global C-function wrapper via `luaJIT_setmode(..., LUAJIT_MODE_WRAPCFUNC)` so C++ exceptions crossing Lua frames are caught in a trampoline. Exactly one wrapper is allowed.

**Threading:** a `lua_State` is **not thread-safe; one VM per OS thread.** Coroutines are cooperative fibers *within* one state, not OS threads. The rule: either drive all Lua from the event-loop thread (simplest, correct), or give each worker thread its own state with no shared objects. Extension work that must be concurrent goes to a subprocess or a separate state — never a shared state.

**Async bridge:** because the VM is fully resumable, an extension can call an async C function that records the current coroutine, yields, and is resumed by the Asio completion handler **on the same thread**. This is the sanctioned pattern; it does not require threads.

## Memory

| Item | Value |
|---|---|
| Library code | ~0.4–0.6 MB (Pall: "a couple hundred kilobytes" for vanilla Lua; LuaJIT ≈ 2× that) |
| Minimum runtime data | ~300 KB (Pall); hello-world peak RSS **2.4 MB** whole-process |
| JIT code cache | `sizemcode=64` KB per area, `maxmcode=2048` KB total; exceeding it causes flush churn |
| GC64 overhead | +8–11% RSS, no CPU cost (`[VENDOR]`, OpenResty) |

LuaJIT's built-in allocator **never returns freed pages to the OS** (OpenResty measured 71% of a 512 MB RSS held by it). Irrelevant for a CLI — process exit reclaims everything — but it is a real consideration if a long-lived server mode ever ships, which is another argument for routing through mimalloc via `lua_newstate`.

Introspection for a `/ext` diagnostic view: `collectgarbage("count")`, `jit.status()`, `luajit -jv` (trace events), `-jdump`, and the built-in low-overhead statistical profiler (`-jp`).

## Traps

- **Pinning a version string instead of a commit.** There are no tags. A recipe that says `2.1.0-beta3` is pinning a 2017 snapshot.
- **Assuming the JIT is free.** For C-boundary-heavy glue it is a measured 2.1× *penalty*. Default it off and enable per-extension.
- **Expecting instruction hooks to fire under JIT.** They do not, without an unsupported build flag. Budget enforcement requires interpreted execution.
- **Throwing C++ exceptions through Lua frames.** Undefined. Use `lua_pcall` boundaries and the optional global wrapper.
- **One `lua_State` shared across threads.** Not thread-safe. One state per thread, or single-threaded.
- **Treating the VM as a sandbox.** LuaJIT's own FAQ states VM-level sandboxing of untrusted code is not realistic and process-level isolation is the only promising approach. See `12`.
- **Compiling with `/EHa` reflexively.** It is needed only to catch Lua errors via `catch(...)`, and it also swallows access violations.
- **Static-linking LuaJIT on Windows with mixed modes.** "Mixed mode is not supported on Windows. And static mode doesn't work well. C modules cannot be loaded, because they bind to `lua51.dll`." Choose static *or* DLL, consistently.
- **Lua 5.4 copy-paste.** No integers, no `//`, no `utf8`, no `<close>`. Write the porting note.
- **Metatable operations from C in hot paths.** They abort traces and flush the code cache — the mechanism behind the JIT penalty above.

## Open questions

- Does the JIT-off default hold for extension-heavy sessions (many small scripts, long-lived process)? The 2.1× measurement was one C-callback workload; re-measure with real extensions.
- Is per-extension JIT opt-in the right granularity, or should it be per-function via `jit.off(fn)`?
- Do we ship precompiled extension bytecode (`luajit -b`)? It cuts parse cost and doubles as an integrity check, but bytecode is not portable across GC64/arch and loading untrusted bytecode is a crash vector.
- Does `LUAJIT_ENABLE_CHECKHOOK` become viable if we ever need budget enforcement under JIT? It is unsupported and costs in tight loops.
- Bus factor 1 on upstream: at what point does OpenResty's `luajit2` become the safer default?

## Sources

- https://luajit.org/luajit.html — overview, license, platform badges
- https://luajit.org/status.html — branches, rolling-release policy, OS/CPU matrix
- https://luajit.org/install.html — MSVC build, GC64 default, static/DLL caveat
- https://luajit.org/extensions.html — dialect, 5.2/5.3 backports, 3.0 syntax backports, resumable VM, C++ exception interop table
- https://luajit.org/faq.html — VM-level sandboxing stance, bytecode danger
- https://luajit.org/ext_c_api.html — `luaJIT_setmode`, WRAPCFUNC wrapper
- https://luajit.org/ext_ffi_semantics.html — FFI semantics (C99 parser, cdata conversions)
- https://luajit.org/ext_ffi_tutorial.html — FFI library loading
- https://github.com/LuaJIT/LuaJIT/commits/v2.1/ — activity (Sep 2026 commits)
- https://github.com/LuaJIT/LuaJIT/issues/1092 — 3.0 scope
- https://github.com/LuaJIT/LuaJIT/issues/1475 — 3.0 syntax extensions
- https://github.com/LuaJIT/LuaJIT/issues/779 — CHECKHOOK, `LUA_MASKCOUNT` guidance
- https://github.com/LuaJIT/LuaJIT/issues/781 — Windows x64 SEH, mcode ±1 GB, `/EHa`
- https://github.com/LuaJIT/LuaJIT/issues/391 — minimum memory numbers
- https://github.com/openresty/luajit2 — downstream fork scope
- https://blog.openresty.com/en/luajit-gc64-mode/ — 2 GB ceiling, GC64 cost
- https://blog.openresty.com/en/luajit-plus/ — allocator RSS retention
- https://marek.vavrusa.com/embedding-luajit/ — `jit.off()` 2.1× measurement, metatable trace flushes
- https://programming-language-benchmarks.vercel.app/lua-vs-c — LuaJIT vs 5.4.7 numbers
- https://luau.org/performance/ — Luau interpreter/JIT characteristics
- https://github.com/microsoft/vcpkg/master/ports/luajit/vcpkg.json — rolling-branch tracking
- https://conan.io/center/recipes/luajit — stale Conan Center version
- https://maki.sh/docs/plugins/ — Lua extension precedent (Luau, Neovim-mirroring API)
- https://github.com/kfcafe/imp — Lua extension precedent (`mlua`)
- https://sol2.readthedocs.io/en/latest/threading.html — one state per thread
