# C++23 Stack

> TL;DR: C++23 features are safe to adopt selectively (skip modules, skip `flat_map`); the runtime stack is Luau + yyjson + **boost::asio** + Beast + Boost.Process v2 + fmt + spdlog + mimalloc + simdutf + unordered_dense under Conan 2; realistic static binary ~3–5 MB without TLS, ~5–9 MB with it.

## Language features

### Adopt

| Feature | Use | Notes |
|---|---|---|
| `std::expected<T, E>` | Every fallible API: file I/O, JSON, tool dispatch, MCP RPC | Value-level errors, no unwind cost. GCC 12 / Clang 16 / MSVC 19.33 |
| `std::optional` + monadics | Config lookups, optional params | Universal |
| `std::string_view` / `std::span` | Parameter passing, zero-copy framing | Now guaranteed trivially copyable |
| `std::format` / `std::print` | Formatting where fmt is not already linked | GCC 14 / MSVC 19.37 / libc++ 18. On Windows GCC needs `-lstdc++exp` |
| `std::filesystem` | Paths, directory iteration | Universal |
| `std::variant` + `std::visit` | The event type (`19`) | Overload-set idiom; no C++26 `inspect` yet |
| `std::jthread` + `std::stop_token` | Worker threads, cancellation | Cooperative cancellation built in |
| `std::generator` | File line iteration, glob results | GCC 14 / MSVC 19.43; **libc++ missing** — feature-test `__cpp_lib_generator` |
| `std::move_only_function` | Callback storage in the event bus | GCC 12 / MSVC 19.32; **libc++ missing** — gate or fall back |
| Concepts | Constraining the few templates we have | Use sparingly |
| `if consteval`, `constexpr` improvements | Compile-time tables | Universal |

### Avoid or restrict

| Feature | Verdict | Reason |
|---|---|---|
| `import std` / modules | **Defer** | MSVC mature; libc++ documents the workflow as experimental; GCC 15 ships the module but does not build it by default; CMake's GNU import-std support is missing. Modules remain viral and tooling lags |
| Deep ranges pipelines (5+ adaptors) | Restrict | Huge template instantiations; libc++ gaps (`views::enumerate`/`stride` only in Clang 23) |
| `std::flat_map` | Avoid | Newest containers; a sorted vector is 20 lines |
| `std::mdspan` | Avoid | No use case in a CLI |
| Hand-rolled `co_await` machinery | Avoid | No std executor; Asio's C++20 coroutines are the mature path and are used where needed |
| Exceptions for control flow | Avoid | `expected` handles expected failures; exceptions stay enabled for library requirements and invariants |

## Library stack

| Component | Choice | Why | Rejected |
|---|---|---|---|
| Scripting | **Luau** (commit-pinned, vendored) | 978.5 KB linked, ~17 KB RSS per extension, and the only option with a designed capability boundary (`17`, `27`). CMake, no external deps, `LUAU_STATIC_CRT=ON` | LuaJIT (smaller but no boundary — cannot make `_G` readonly or remove `io`/`os`), Lua 5.4 (same), QuickJS (MSVC friction) |
| JSON | **yyjson 0.13** | Fastest dynamic-DOM parser (1.1–1.8 GB/s `[VENDOR]`), mutable DOM via `mut_copy`, JSON Pointer/Patch/Merge-Patch, one `.c`, ANSI C, ~50–100 KB | **glaze** (compile-time typed — wrong for runtime-arriving tool schemas), **nlohmann** (81 MB/s, ~15× slower roundtrip), **rapidjson** (stale) |
| JSON (bulk) | **simdjson — not yet** | Only pays for multi-MB documents with known schema; padded-buffer ceremony and 89 MB/s on out-of-order keys make it wrong for tool payloads | Revisit for bulk file ingestion only |
| Async I/O | **boost::asio** (from Boost 1.91) | Same API as standalone Asio, and it is the one Beast and Process v2 are written against. Taking standalone Asio *as well* would put two Asio implementations and two incompatible `io_context` types in one binary, so Boost's Asio is the single one. Mature C++20 coroutines (`co_spawn`, `awaitable`); one `io_context` + threads + strands | **standalone Asio** (rejected: duplicates boost::asio, which Beast and Process v2 already require), **libuv** (wrong abstraction level), **io_uring** (Linux-only) |
| HTTP/SSE | **Boost.Beast** | HTTP/1.1 + SSE client on the Asio executor we already have — one event loop, zero extra deps. ~50 lines of SSE line-parsing is unavoidable in any C++ client | **libcurl** (compiled C dep, but the pragmatic choice if proxy/HTTP2 requirements appear), **raw Asio** (reimplementing chunked encoding) |
| Subprocess | **Boost.Process v2** | Asio-pipe based, UTF-8 on Windows via W APIs, `pidfd_open` on Linux, fd-safe by default, structured stop sequences | **reproc** (simplest correct alternative if Boost is dropped), **hand-rolled CreateProcess** (UTF-16 quoting is a trap) |
| Formatting | **fmt 12.2** | Kept as long as spdlog is kept (spdlog is fmt-based). `std::print` is viable but fmt's chrono/ranges/color headers have no std equivalent | Drop both later if spdlog migrates to `use_std_fmt` |
| Logging | **spdlog 1.17** | Async queue (never block the loop on file I/O), rotating sinks, Windows debug-output sink, pattern formatting | Hand-rolled (~100 lines, but then we rebuild sinks under time pressure) |
| Hash maps | **ankerl::unordered_dense 5.2** | Iteration 5–13× faster than flat maps, ~110× `std::unordered_map` `[VENDOR]`. Cost: iterators *and* references invalidated on insert/erase | `std::unordered_map` (slow), absl (heavy dep) |
| Allocator | **mimalloc 3.4+** | Drop-in, Windows static override now works (`/MT` + `mimalloc.obj` first on the link line), `mimalloc-new-delete.h` overrides global new/delete in one TU | jemalloc/tcmalloc (build complexity) |
| Unicode | **simdutf 9.2** | UTF-8 validation, UTF-8↔UTF-16 transcode for Win32 wide APIs, base64 for data URLs. Trimmed amalgamation. Node/Chromium/ghostty-proven | Hand-rolled UTF-8 (error-prone) |
| State storage | **SQLite + FTS5 — not yet** | JSONL session files first (`09`, `16`); SQLite only when persistent search actually hurts | Premature |
| Build | **CMake + Ninja** | Ecosystem ubiquity, presets, Conan toolchain integration | Meson (better config language, second-class vcpkg/Conan toolchain), xmake |
| Deps | **Conan 2.32** | Lockfiles and reproducibility (vcpkg has neither), `CMakeToolchain` emits presets, `CMakeDeps` generates `find_package` configs | vcpkg (no lockfiles) |

**Luau packaging:** upstream ships CMake with no external dependencies, so the private Conan recipe in `conan/recipes/luau/` is a thin wrapper that pins a **commit** (`17`). Two build variants matter: `Luau.VM` alone (bytecode in, no parser) or `Luau.VM` + `Luau.Compiler` (source in). `A5` measures both; the VM-only variant is a real size lever.

## Streaming SSE

SSE is a long-lived HTTP/1.1 response with `text/event-stream`; the client parses `data:` / `event:` / `id:` / `retry:` lines and reconnects with `Last-Event-ID`. **No C++ library ships a conforming SSE client** — everyone hand-rolls the ~50-line line parser.

Beast path: send the request, then incremental `async_read_some` into a `flat_buffer` (or `async_read_until(stream, buf, "\n\n")`), feeding an incremental line parser. This shares the executor with subprocess pipes and timers, which is the reason to choose it over libcurl.

Two things that bite: sequences split across reads (never assume a full event per read), and providers that emit mid-stream errors after HTTP 200.

## Error model

```cpp
enum class errc { io, json, protocol, tool_failed, cancelled, lua_error };
struct error { errc code; std::string msg; };
template<class T> using result = std::expected<T, error>;
```

Never throw across tool boundaries or Lua frames (`17`). Exceptions stay enabled for library requirements and true invariants.

## Threading

One `io_context`, a small thread pool calling `run()`, strands for serialization. Main thread drives the event loop and the single `lua_State` (`17`). Subprocess stdout/stderr pumped concurrently via async pipes; **close stdin explicitly** when done writing, or the child blocks forever waiting for EOF.

No executor abstraction, no coroutine scheduler of our own. Asio coroutines are used where they simplify streaming, not as a universal style.

## ConPTY bridge

ConPTY requires **synchronous** I/O on dedicated threads — overlapped I/O deadlocks. Bridge with a reader `jthread` (blocking `ReadFile` → queue → loop) and a writer `jthread` (queue → blocking `WriteFile`). POSIX uses `forkpty` behind the same interface. This is one place where threads beat coroutines, because the API demands it.

## Build configuration

- **CMake ≥ 3.29 + Ninja**, `CMakeToolchain` + `CMakeDeps` from Conan 2, lockfiles committed.
- **Toolchains:** Windows MSVC 19.4x (`/std:c++23`, `/permissive-`, `/utf-8`); Linux GCC 14+ or Clang 18+ with libstdc++ (libc++ C++23 gaps make it the worse default); macOS Apple Clang 15+ with the Xcode SDK (`std::generator` and `std::move_only_function` are missing from libc++ — feature-test macros are mandatory, `24`).
- **Linking, per platform** (`24`): Windows `/MT` fully static; Linux **musl static** (the fix for glibc skew); macOS **cannot be fully static** — Apple requires a dynamic `libSystem`, so the promise there is "no bundled third-party libraries", not "one file". LTO on release; `-ffunction-sections -fdata-sections` + `--gc-sections`.
- **Startup path:** no global constructors (audit `.init_array`), lazy-init SQLite/logging/Lua, `-O2` over `-Os` (startup is page-fault-bound, not size-bound).
- **PGO is not assumed to help startup** — measured gains are 0–5% for typical binaries; the large wins come from post-link layout (BOLT-style), which is only worth it if profiling shows page-fault dominance.
- **Construct the spdlog async queue lazily.** Its default 8192 × 272 B ≈ 2.18 MB allocation at construction is a real cost on a <15 ms startup budget.

## Binary size

| Component | Estimate |
|---|---|
| Luau (static, VM + compiler) | 978.5 KB measured (`27`) |
| Asio + Beast instantiated code | 0.8–2.0 MB |
| fmt + spdlog (compiled) | 0.3–0.6 MB |
| simdutf (trimmed) + yyjson + unordered_dense | 0.2–0.5 MB |
| mimalloc | 0.1–0.2 MB |
| **Subtotal** | **~2.9–5.0 MB** |
| + static OpenSSL 3 (if TLS is static) | +2–4 MB → **~5–9 MB** |

TLS is the single biggest variable and decides whether the ≤25 MB budget (`01`) is comfortable or tight. Per-component figures are cross-source planning estimates, not measurements — validate with a link map.

## Traps

- **Header-only compile-time cost.** Asio measured **~0.7–1.2 s per TU** and Beast worse (a whole static Beast-SSL client was 6 s to compile vs 100 ms for libcurl). Include them only in dedicated network TUs; never from widely-included headers. Use `BOOST_ASIO_SEPARATE_COMPILATION`.
- **`FMT_HEADER_ONLY` and header-only spdlog** multiply the above across every TU. Use compiled mode.
- **Beast is not an HTTP client.** No HTTP/2, no URL parsing, no redirects, no connection pool, no cookie handling. You build those, or you use libcurl.
- **Blocking `execute()` with live pipes deadlocks.** The classic Windows full-pipe deadlock; always pump stdout *and* stderr concurrently and close stdin.
- **Metatable/userdata churn from C aborts JIT traces** and flushes the code cache — the mechanism behind the JIT penalty (`17`).
- **`std::generator` is single-shot**; libc++ lacks it entirely.
- **`move_only_function` invocation on an empty target is UB** (strong precondition, no throw).
- **unordered_dense invalidates references on insert**, not just iterators. Never hold a pointer into it across a mutation.
- **The VM's allocator is ours to set.** Luau routes every allocation through `lua_Alloc`, which is how the per-extension ceiling and `ext doctor` attribution work. Back it with mimalloc and count in the shim.
- **`LUAU_STATIC_CRT=ON` is required to match our `/MT`.** Mismatching the CRT across the VM and the host is a link error at best and a heap-corruption bug at worst.
- **Static glibc pulls NSS/DNS machinery** (6–15 MB) and breaks the resolver; musl lands 2–7 MB with no external deps. On Windows/macOS this does not apply.
- **macOS arm64 binaries must be signed** — ad-hoc is sufficient, but post-link tools (`strip`, `install_name_tool`) invalidate the signature, so re-sign last in the pipeline (`24`).
- **Two TLS stacks** if libcurl and Asio/Beast both land with different backends. Pick one.

## Open questions

- TLS backend: Beast + OpenSSL (2–4 MB) versus libcurl (one dependency, proxies and HTTP/2 free). This decides the binary budget.
- Does the VM-only (precompiled bytecode) variant save enough to justify the signing requirement and losing source review? `A5` measures it.
- ~~Do we need Boost at all if we take standalone Asio + reproc?~~ **Resolved: keep Boost.** Beast (HTTP/SSE) and Boost.Process v2 both require `boost::asio`, so Boost is a dependency regardless of the subprocess choice, and adding standalone Asio on top would duplicate it. Note for implementers: **Boost.Process v2 is not header-only** — it ships compiled sources, so Boost must be built with `header_only=False`.
- Does `std::print` alone suffice once spdlog's `use_std_fmt` matures, letting us drop fmt?
- ConPTY on Windows ARM64: verified path?

## Sources

- https://en.cppreference.com/w/cpp/compiler_support — C++23 feature × compiler matrix
- https://gcc.gnu.org/onlinedocs/libstdc++/manual/status.html — libstdc++ status
- https://libcxx.llvm.org/Status/Cxx23.html + /Modules.html — libc++ status, module workflow
- https://learn.microsoft.com/en-us/cpp/cpp/modules-cpp — MSVC modules
- https://www.kitware.com/import-std-in-cmake-3-30/ — CMake `import std` support
- https://github.com/ibireme/yyjson — yyjson features, mutable DOM, JSON Pointer/Patch
- https://raw.githubusercontent.com/stephenberry/json_performance/master/README.md — cross-library JSON benchmarks
- https://ibireme.github.io/yyjson/doc/doxygen/html/data-structures.html — mutable DOM internals
- https://raw.githubusercontent.com/simdjson/simdjson/master/README.md — on-demand limits, padded input
- https://think-async.com/Asio/ — standalone Asio
- https://www.boost.org/doc/libs/latest/doc/html/boost_asio/using.html — Asio compilation modes, coroutine support
- https://artificial-mind.net/assets/compile-health/compile-health-data.json — measured Asio include cost
- https://github.com/boostorg/beast/issues/2117 — Beast vs libcurl compile time and binary size
- https://www.boost.org/doc/libs/1_88_0/libs/process/doc/html/index.html — Boost.Process v2
- https://github.com/boostorg/process/issues/64 — pipe deadlock diagnosis
- https://devblogs.microsoft.com/oldnewthing/20110707-00/?p=10223 — Windows pipe redirection pitfalls
- https://raw.githubusercontent.com/fmtlib/fmt/master/README.md — fmt bloat methodology
- https://raw.githubusercontent.com/martinus/unordered_dense/master/README.md — perf and stability caveats
- https://raw.githubusercontent.com/microsoft/mimalloc/master/readme.md — override mechanisms
- https://raw.githubusercontent.com/simdutf/simdutf/master/README.md — capabilities, size
- https://raw.githubusercontent.com/gabime/spdlog/v1.x/README.md — async queue memory, sinks
- https://docs.conan.io/2/reference/tools/cmake/cmaketoolchain.html — CMakeToolchain
- https://docs.conan.io/2/reference/tools/cmake/cmakedeps.html — CMakeDeps
- https://github.com/luau-lang/luau — CMake build, no external dependencies, `LUAU_STATIC_CRT`
- https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session — ConPTY synchronous I/O
- https://www.dag.inf.usi.ch/wp-content/uploads/cgo25.pdf — profile-guided layout gains
