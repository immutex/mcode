# mcode

An extensible C++23 coding-agent harness with a Luau extension layer.

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
./build/Release/bin/mcode.exe
```

```bash
# Linux / macOS
python3 -m pip install "conan==2.32.0"
./scripts/bootstrap.sh
cmake --build build/Release
ctest --preset linux-gcc        # or macos-clang
./build/Release/bin/mcode
```

The bootstrap script is not a convenience wrapper — it does three things that are
easy to get wrong by hand: it activates the MSVC developer environment, builds
the private Luau package, and selects the right Conan profile for the platform.

## Layout

| Path | Contents |
|---|---|
| `docs/` | The design. Start at `docs/00-index.md` |
| `src/mcode/` | Library sources, mirroring `docs/03`'s layer model |
| `src/main.cxx` | Startup smoke test: exercises every dependency |
| `tests/` | Catch2 unit tests |
| `cmake/` | Warning set, platform configuration |
| `conan/profiles/` | Per-platform Conan profiles |
| `conan/recipes/luau/` | Private Luau recipe (see below) |
| `scripts/` | Bootstrap |

Source tree:

```
src/mcode/
├── core/       error model, tool registry, version
├── support/    logging (spdlog), JSON (yyjson), Unicode (simdutf)
├── ext/        the Luau extension host
├── fs/         workspace boundary and file primitives
├── net/        SSE parser, HTTP client (Beast)
├── proc/       subprocess (Boost.Process v2)
└── agent/      loop, session budget, event log
```

Dependencies point downward only, from `agent/` toward `core/`.

## Dependencies

Everything comes from Conan 2 except Luau, which mcode builds from its own
recipe. See `docs/14-cpp23-stack.md` for why each was chosen.

| Component | Version | Role |
|---|---|---|
| Luau | `0.0.0-mcode.c0e346ed` (commit `c0e346ed`) | Extension layer |
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

8. **Luau is static-only, deliberately.** Upstream gates `LUAU_BUILD_SHARED`
   behind `LUAU_EXTERN_C`, which force-enables `LUA_USE_LONGJMP=1` — that changes
   how `luaL_error` and the panic handler propagate, from C++ exceptions to
   `longjmp`. mcode's host catches VM errors as C++ exceptions, so the shared
   configuration is a different API contract rather than a packaging variant.
   `LUAU_STATIC_CRT` follows the consumer's CRT: mismatching it across the VM and
   the host is heap corruption, not a link error.

9. **No OpenSSL, so no TLS.** `docs/14`'s open question *"TLS backend: Beast +
   OpenSSL versus libcurl"* changes the binary budget by 2–4 MB, so it is left
   open. `http_client` returns `errc::unsupported` for `https://` rather than
   silently downgrading — a silent downgrade would be a security bug.

10. **Conan must build the VM with the same toolset as the consumer.** Two MSVC
    installations on this machine resolve differently: `vswhere -latest` returns
    VS18 Community (cl 19.51, Conan `compiler.version=195`) while VS2022
    BuildTools is cl 19.44 (`194`). Building the Conan packages with one and the
    consumer with the other links against a different STL and fails with
    unresolved `__std_*` symbols. The profile's `compiler.version` must match
    whatever CMake's generator picks — check both before trusting a link error.

11. **The extension API surface must be complete before the VM is sealed.**
    Luau checks `readonly` on every C API write path, so `luaL_sandbox` is a
    one-way door: after it, the host cannot add an API entry either. This is why
    `docs/18` freezes the surface rather than growing it at runtime.

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

16. **A raw string literal containing `)"` terminates early.** The delimiters are
    `R"( … )"`, so a Luau or JSON payload that ends with a quote-paren closes the
    literal and the compiler reports a syntax error hundreds of lines later, or
    reports the raw string as unterminated at end of file. Use a custom
    delimiter: `R"JSON( … )JSON"`, `R"LUASRC( … )LUASRC"`. This has cost time
    three separate times; it is now the first thing to check when a payload
    literal misbehaves.

17. **`luaL_*` lives in `lualib.h` for Luau, not `lauxlib.h`.** Upstream Lua puts
    `luaL_checkstring` and friends in `lauxlib.h`; Luau's Conan package ships
    `lua.h`, `luacode.h`, `luaconf.h`, and `lualib.h` and no `lauxlib.h` at all.

18. **The `edit` tool can no-op silently.** A fuzzy whitespace mismatch is
    reported as success while the file is unchanged. Verify with a read or a
    grep after any edit that matters; a test that keeps passing after an edit is
    a symptom, not a reassurance.

19. **A `lua_host` must not be moved after the API surface is installed.** The
    surface stores a raw `lua_host*`, so moving the host into a `unique_ptr`
    afterwards leaves the pointer dangling and every tool call fails with "the
    host has no thread" — long after the code that caused it. The loader
    constructs the VM into its `unique_ptr` directly and never moves it again.

20. **A `hook_registry` must be declared after the `events::bus` it binds.** It
    unsubscribes in its destructor, so a bus destroyed first leaves the registry
    unsubscribing from freed memory — a crash at scope exit, nowhere near the
    code that caused it. The bus is bound at construction so the requirement is
    in the type rather than in a comment.

21. **Luau `--!strict` requires an explicit return on every codepath.** A handler
    whose declared return is `{ veto: string }?` must end with `return nil` when
    it does not veto. Declaring the return `any` would silence this, at the cost
    of letting a misspelled `{veto = …}` compile — a guard the author believes is
    active but that never blocks. The explicit `return nil` is the cheaper side.

22. **`conan --profile` sets only the HOST context.** The build profile still
    defaults to `~/.conan2/profiles/default`, which a fresh CI runner does not
    have — so `conan create --profile <file>` fails with "The default build
    profile ... doesn't exist" on every platform. `conan profile detect --force`
    must run first. Confirmed against `conan create --help`.

23. **`luau_load` takes BYTECODE, not source.** Handing it a `.luau` file makes
    it read the first byte as a bytecode version and fail with
    `bytecode version mismatch (expected [3..14], got 114)`. `luau_compile` must
    run first, and its result is what `luau_load` consumes. `lua_host::run`,
    `eval_to_string` and the `require` loader all go through one `load_chunk`
    helper for this reason — the two hand-written copies had already drifted.

24. **`WaitForSingleObject` on a process needs `SYNCHRONIZE` access.**
    `OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION )` is enough for
    `GetExitCodeProcess` but makes the wait fail immediately, so every live
    process reports as dead. Ask for both rights. The exit-code probe is not an
    alternative: a process that terminated with 259 reports as `STILL_ACTIVE`
    forever.

25. **`yyjson_obj_iter_next` yields the KEY.** The value is reached with
    `yyjson_obj_iter_get_val( key )`. Using the iterator's return as a value
    compiles, because both are `yyjson_val*`, and silently produces a list of
    keys where values were expected.

26. **A `docs/NN` citation in a code comment is banned, and prose around it
    breaks when the citation is removed.** Stripping `(docs/22 E4)` from
    `// ... the headless surface (docs/22 E4).` is safe; stripping
    `docs/22. A consumer script branches on these, so the` leaves a sentence
    with no subject. Read the whole comment, not the match.

23. **LuaJIT's Makefile refuses to build on Darwin without
    `MACOSX_DEPLOYMENT_TARGET`.** It must be *exported*, not passed as a make
    variable: the check reads the environment. The error is
    `*** missing: export MACOSX_DEPLOYMENT_TARGET=XX.YY`.

24. **`conan/profiles/windows-msvc` targets MSVC 194, not this machine's 195.**
    A bare `compiler.version` is read as a *Visual Studio generation* number, so
    195 means "Visual Studio 18" and the recipe generator fails with
    `VS non-existing installation: Visual Studio 18` on any runner without it.
    The profile carries the portable value plus
    `tools.microsoft.msbuild:vs_version=17`; override on the command line for a
    local VS 18 toolset.

25. **Conan's Apple Silicon arch is `armv8`, not `arm64`.** `arch=arm64` fails
    with `Invalid setting 'arm64' is not a valid 'settings.arch' value`.

26. **GCC's `-Wshadow` and `-Wmissing-field-initializers` fire under
    `-DMCODE_WARNINGS_AS_ERRORS=ON` and MSVC does not have them.** A designated
    initializer that leaves a field defaulted, and a local shadowing a member,
    both compile clean on MSVC and fail the Linux leg. The Linux job is the only
    thing that catches these.

27. **A designated initializer that leaves a field defaulted fails the GCC and
    Clang legs.** `-Wmissing-field-initializers` is on under
    `-DMCODE_WARNINGS_AS_ERRORS=ON`, and MSVC has no equivalent. Construct the
    struct and assign the fields, or initialize every member.

28. **`[[nodiscard]]` on an infallible helper is a defect, not a safety net.**
    MSVC's C4834 is also an error here, so a `status`-returning function that
    never fails forces every caller either to check a value that is always empty
    or to discard it. `provider.cxx`'s `string_member` returned `status` and now
    returns `void`; the three call sites were the only reason the warning fired.

29. **An unused file-scope function is an error on GCC and Clang.** MSVC does not
    warn. When the only reader of a registry slot is such a function, the slot is
    dead too — remove both, do not suppress the warning.

30. **Nothing loaded may point into the loader's per-candidate locals.** The
    loader builds a `manifest` per directory inside the loop and destroys it at the
    end of the iteration, while the `api_surface` it installed lives on and reads
    `manifest_.name` on every `mcode.log.*` call. Keeping a `const manifest*`
    there is a use-after-free whose symptom depends on stack layout: it crashed
    only after an unrelated signature change, and resolving it needed the linker
    map. The surface now OWNS a copy — a manifest is a handful of small strings.

31. **Run `python _clgate.py` before pushing anything that touches C++.** MSVC is
    the only compiler installed here, so `-Wshadow`, `-Wsign-compare`,
    `-Wunused-parameter`, `-Wconversion`, and `-Wunused-private-field` are all
    invisible locally and cost a ten-minute CI round each. The gate compiles the
    changed units with the local clang++ against the real Conan include paths,
    using the same warning set as the CI Clang leg. It takes seconds.

32. **An `if` and its `else if` share a scope.** Two `if (const auto* x = ...)`
    declarations with the same name in one if/else-if chain is `-Wshadow`, and
    renaming both to a new shared name does not fix it — the second declaration
    still shadows the first. This was made twice in a row before the local gate
    was written.

33. **`lua_objlen` returns `int`, not `size_t`.** Every comparison against a
    `size_t` counter is then a sign-compare. Convert once at the source.

34. **`-Wnull-dereference` is deliberately not in the warning set.** GCC emits it
    from inside `boost/asio/io_context.hpp` and
    `boost/beast/http/impl/fields.hpp` under `-O3` inlining, where the
    system-header suppression does not survive the optimizer. Every occurrence in
    this project is Boost's code; the flag buys a build failure we cannot fix.

35. **`std::filesystem::contains`-style comparison must canonicalize its input.**
    Windows spells one directory two ways, and a CI runner's `%TEMP%` is the 8.3
    short form (`RUNNER~1`). The canonical root is the long form, so comparing a
    raw short path component-wise rejects a directory that is plainly inside the
    workspace. `weakly_canonical` resolves the existing prefix and leaves the rest
    lexical, so a path that does not exist yet still compares correctly. The
    regression test builds a long directory name, takes its short form via
    `GetShortPathNameW`, and asserts both spellings are accepted.

36. **The executable cannot be named `src/mcode`.** That path is the SOURCE
    DIRECTORY, and on Linux and macOS the executable has no suffix, so the linker
    tries to open a directory for writing: `ld: cannot open output file src/mcode:
    Is a directory`. Windows hid this behind `.exe` for the project's whole life.
    Runtime output is `${CMAKE_BINARY_DIR}/bin` on every platform.

37. **`kill( 0, 0 )` and `kill( -1, 0 )` both SUCCEED.** Neither is a pid query:
    0 means the caller's process group and -1 means every process the caller may
    signal, so an unvalidated pid reports as alive — and the same value passed to
    a real kill would broadcast. `process_is_alive` validates the pid before the
    syscall. Windows hides this entirely: `OpenProcess` simply fails for a bogus
    id.

38. **`__cpp_lib_expected` is defined by `<expected>`, not by the language.**
    Testing the macro without including the header reports "std::expected is
    required" on a toolchain that has it.

39. **A gate metric must not depend on how the tool was invoked.** The bench's
    event counts are `iterations` by construction, and the iteration count is a
    command-line argument — so gating them against a fixed `expected = 50000`
    encoded the invocation, not the behaviour, and failed on two platforms the
    moment CI passed 20000. The gate is on the RATIO (events per iteration,
    expected exactly 1.0), which is invocation-independent and still catches an
    applier that drops events.

40. **A feature-test macro is defined by its own header.** `__cpp_lib_expected`
    comes from `<expected>`, `__cpp_lib_generator` from `<generator>`. Testing
    them without the include reports "not available" on a toolchain that has
    them — which is exactly what `detect_library_support` did on GCC 14 and
    Apple Clang 16. Optional headers are pulled in behind `__has_include`.

41. **`catch_discover_tests` does not map Catch2 tags to ctest labels unless
    `ADD_TAGS_AS_LABELS` is passed.** Without it, `ctest -L platform` finds zero
    tests and fails with exit 8 — on a suite that is entirely green. The CI matrix
    runs exactly that command for the per-platform seam check.

42. **A timing ceiling must come from a documented budget, not from a local
    measurement.** `load_per_ext_us` was gated at 250 us — 2.3x the ~110 us
    measured on a quiet machine — and a shared runner produced 341 us. docs/28
    already records that timing metrics vary 26–131%; a gate tight enough to be
    interesting is a gate that fails on load. It is the 1 ms budget from docs/01
    now, which still catches an order-of-magnitude regression.

43. **`grep` is not on PATH in a plain `cmd.exe`.** The local gate filters
    compiler output with `findstr` on Windows and `grep` elsewhere, chosen per
    platform rather than assumed. A `|| true` on a cmd pipeline does not suppress
    "command not found" either, so the filter would silently report nothing.

44. **Sorting before comparison is not optional in reports.** Extension discovery
    sorts by name, and JSON object keys are emitted in sorted order, so two runs
    are byte-identical. An unsorted report makes a duplicate-name or load-order
    failure unreproducible.

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
$ ./build/Release/bin/mcode.exe
== spdlog (logging) ==
== C++23 library support ==
== yyjson (JSON) ==
== simdutf (Unicode) ==
== ankerl::unordered_dense (tool registry) ==
== Luau (extension layer) ==
== Luau boundary (every escape must fail) ==
== Beast (SSE line parser) ==
== Boost.Process v2 (subprocess) ==
== filesystem (workspace boundary) ==
== agent loop (budget + event log + dispatch) ==
  88 checks, 0 failures
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

mcode embeds **Luau**, which is distributed under the MIT License. Luau is
Copyright (c) 2019-2025 Roblox Corporation and Copyright (c) 2005-2019 Lua.org,
PUC-Rio. The full text ships in the Conan package under `licenses/` as
`LICENSE.txt` and `lua_LICENSE.txt`, and is reproduced in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

Upstream asks that products embedding Luau carry attribution for the language
and a link to <https://luau.org/> in their documentation. This section and the
notices file satisfy that request.
