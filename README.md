# mcode

An extensible C++23 coding-agent harness with a LuaJIT extension layer.

**Status: scaffold.** This is the project skeleton — build system, dependency set,
and a smoke test that exercises every library. There is no agent behaviour yet.
The design lives in [`docs/`](docs/00-index.md), which is the authoritative
specification; where this scaffold and the docs disagree, the docs win.

## Quick start

```powershell
# Windows
python -m pip install "conan==2.32.0"
pwsh -File scripts/bootstrap.ps1
cmake --build build/Release
ctest --preset windows-msvc
./build/Release/src/mcode.exe
```

```bash
# Linux / macOS
python3 -m pip install "conan==2.32.0"
./scripts/bootstrap.sh
cmake --build build/Release
ctest --preset linux-gcc        # or macos-clang
./build/Release/src/mcode
```

The bootstrap script is not a convenience wrapper — it does three things that are
easy to get wrong by hand: it activates the MSVC developer environment, builds
the private LuaJIT package, and selects the right Conan profile for the platform.

## Layout

| Path | Contents |
|---|---|
| `docs/` | The design. Start at `docs/00-index.md` |
| `src/mcode/` | Library sources, mirroring `docs/03`'s layer model |
| `src/main.cxx` | Startup smoke test: exercises every dependency |
| `tests/` | Catch2 unit tests |
| `cmake/` | Warning set, platform configuration |
| `conan/profiles/` | Per-platform Conan profiles |
| `conan/recipes/luajit/` | Private LuaJIT recipe (see below) |
| `scripts/` | Bootstrap |

Source tree:

```
src/mcode/
├── core/       error model, tool registry, version
├── support/    logging (spdlog), JSON (yyjson), Unicode (simdutf)
├── ext/        the LuaJIT extension host
├── fs/         workspace boundary and file primitives
├── net/        SSE parser, HTTP client (Beast)
├── proc/       subprocess (Boost.Process v2)
└── agent/      loop, session budget, event log
```

Dependencies point downward only, from `agent/` toward `core/`.

## Dependencies

Everything comes from Conan 2 except LuaJIT, which mcode builds from its own
recipe. See `docs/14-cpp23-stack.md` for why each was chosen.

| Component | Version | Role |
|---|---|---|
| LuaJIT | `2.1.0-mcode.1` (commit `c6ffc141`) | Extension layer |
| yyjson | 0.12.0 | JSON |
| Boost | 1.91.0 | Beast (HTTP/SSE), Process v2 (subprocess) |
| fmt / spdlog | 12.1.0 / 1.17.0 | Formatting, logging |
| mimalloc | 3.5.1 | Allocator |
| simdutf | 9.0.0 | UTF-8/UTF-16 |
| ankerl::unordered_dense | 5.0.1 | Tool registry |
| Catch2 | 3.16.0 | Tests |

### Non-obvious constraints

Each of these cost real time to discover and are recorded here rather than in
code comments.

1. **No standalone Asio.** `docs/14` lists standalone Asio *and* Boost, but Beast
   and Boost.Process v2 are both written against `boost::asio`. Taking both would
   put two Asio implementations and two incompatible `io_context` types in one
   binary. mcode uses `boost::asio` as its single Asio, which resolves `docs/14`'s
   open question *"Do we need Boost at all if we take standalone Asio + reproc?"*
   in favour of Boost.

2. **`BOOST_ASIO_SEPARATE_COMPILATION` is unusable here.** `docs/14` §Traps
   recommends it to keep Asio's implementation out of every TU. But Boost's
   compiled libraries are themselves built with separate compilation enabled, so
   `libboost_process` already contains Asio's implementation objects
   (`get_misc_category` and friends). Enabling it in mcode too, plus an
   `asio/impl/src.hpp` TU, defines those symbols twice:
   `LNK2005: get_misc_category already defined`. Since Process v2 has no
   header-only mode, there is no way out. Asio runs header-only; the cost is
   compile time, not correctness.

3. **Boost is not header-only.** Beast and Asio are; **Boost.Process v2 is not**.
   It ships compiled sources (`detail::terminate_`, `check_running_`,
   `windows::default_launcher::…`), so a header-only Boost links with 20
   unresolved externals. `conanfile.py` builds it with `header_only=False` and
   disables the components mcode does not use purely to cut build time.

4. **`BOOST_PROCESS_USE_STD_FS` is defined globally.** Process v2 defaults to
   `boost::filesystem`, a compiled library that would duplicate the
   `std::filesystem` the project already uses.

5. **`BOOST_ALL_NO_LIB` is defined globally.** Boost's MSVC autolink emits
   `#pragma comment(lib, …)` for components mcode does not link. Components are
   named explicitly instead (`Boost::process`, `Boost::boost`), so a missing one
   surfaces as an unresolved external rather than a missing file.

6. **`yyjson` 0.12 has no sort-on-write flag.** `docs/05` requires fixed key
   order for cache stability, so `json::document` keeps its members in a
   `std::map` and builds the yyjson tree at `dump()` time. Do not replace that
   with insertion-order emission.

7. **fmt is pinned to 12.1.0, not `docs/14`'s 12.2.** `spdlog/1.17.0`'s recipe
   requires exactly 12.1.0 and Conan rejects the conflict. Since `docs/14` keeps
   fmt only because spdlog is fmt-based, spdlog's pin wins.

8. **LuaJIT is static-only on Windows.** `msvcbuild.bat` compiles the DLL build
   with `/MD` while mcode uses `/MT`; mixing runtimes is undefined behaviour
   rather than a link error, so the recipe refuses the combination. The static
   build's `LJCOMPILE` sets no runtime flag, so it takes cl.exe's `/MT` default —
   which is exactly what is wanted.

9. **No OpenSSL, so no TLS.** `docs/14`'s open question *"TLS backend: Beast +
   OpenSSL versus libcurl"* changes the binary budget by 2–4 MB, so it is left
   open. `http_client` returns `errc::unsupported` for `https://` rather than
   silently downgrading — a silent downgrade would be a security bug.

10. **`msvcbuild.bat` exits 0 on failure.** Its failure path prints an error and
    falls through to `:END`. The recipe asserts `lua51.lib` exists, because
    otherwise a broken build produces a headers-only package that installs
    cleanly and fails much later at link time.

11. **`VCVars` must run in the Conan `generate` phase, not `build`.** It writes
    `conanvcvars.bat` *and* appends a call to it from `conanbuild.bat`, which
    `self.run()` sources. Called from `build()`, the append is lost and
    `msvcbuild.bat` fails with "You must open a Visual Studio Command Prompt".

12. **The subprocess timeout timer must be cancelled when both pipes hit EOF.**
    Otherwise `io_context::run()` blocks until the full timeout expires even
    though the child already exited — every fast command then takes the whole
    timeout. A shared outstanding-read count drives the cancel.

13. **A file ending in `\n` has no trailing empty line.** Line splitting uses
    `start < size`, not `start <= size`, so counting matches `wc -l`. Getting
    this wrong shifts every line number the agent sees.

14. **Windows needs the MSVC developer environment.** `cl.exe` is only on `PATH`
    inside one. A MinGW toolchain on the same machine ships a `link.exe` that
    shadows MSVC's and produces confusing link errors; `bootstrap.ps1` checks for
    it.

15. **`mimalloc-new-delete.h` may be included in exactly one translation unit.**
    That TU is `src/mcode/support/mimalloc_override.cxx`; it overrides global
    `operator new`/`delete`. On Windows the static override works only when
    mimalloc's object precedes the CRT on the link line, which the Conan target's
    interface link libraries arrange — do not reorder `mcode_mimalloc` in
    `target_link_libraries`.

## Building

```bash
cmake --preset windows-msvc   # or linux-gcc / macos-clang
cmake --build build/Release
ctest --preset windows-msvc
```

Presets are defined in `CMakePresets.json` and consume the Conan toolchain, so
`conan install` must run first. `-DMCODE_WARNINGS_AS_ERRORS=ON` for a strict
build, and the `dev` preset turns LTO off for faster iteration.

### Windows note

MSVC is only on `PATH` inside a developer prompt. Either run
`scripts/bootstrap.ps1` (which activates it) or start a "Developer PowerShell for
VS". A MinGW toolchain on the same machine will otherwise shadow the linker and
produce confusing errors.

## Verification

The smoke test is the scaffold's proof that the toolchain works end to end:

```
$ ./build/Release/src/mcode.exe
== spdlog (logging) ==
== C++23 library support ==
== yyjson (JSON) ==
== simdutf (Unicode) ==
== ankerl::unordered_dense (tool registry) ==
== LuaJIT (extension layer) ==
== Beast (SSE line parser) ==
== Boost.Process v2 (subprocess) ==
== filesystem (workspace boundary) ==
== agent loop (budget + event log + dispatch) ==
  58 checks, 0 failures
```

Its exit code is the number of failed checks, so CI gates on it directly.

## Platform support

Windows, Linux, and macOS are all tier-1 (`docs/24`). CI runs the full suite on
Linux and Windows, and build + smoke on macOS.

| Platform | Toolchain | Linking |
|---|---|---|
| Windows x64 | MSVC 19.4x | `/MT`, fully static |
| Linux x86_64 | GCC 14+ or Clang 18+ (libstdc++) | musl static for release |
| macOS arm64 | Apple Clang 16+ | Cannot be fully static — Apple requires a dynamic `libSystem` |

## License

Apache-2.0.
