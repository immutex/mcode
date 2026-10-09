# Non-obvious constraints

> TL;DR: 128 things that cost real time to discover. Docs and `AGENTS.md` cite them by number, so numbering is stable: a constraint keeps its number, and a retired one leaves a gap rather than renumbering the rest. New entries append at the end.

## Constraints

Read the number you were sent to, not the whole file. Each entry states the constraint, then why it exists. If the code and the entry disagree, one of them is a bug.

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


9. **TLS is Beast + OpenSSL, static, verification ON.** `docs/14`'s open
   question *"TLS backend: Beast + OpenSSL versus libcurl"* is resolved in
   favour of OpenSSL: Beast is already the HTTP stack, and libcurl would add a
   second one. `openssl/*:shared=False` in `conanfile.py` keeps the
   no-third-party-runtime-dependencies budget honest — a dynamic `libcrypto`
   fails at *runtime* on a clean machine, not at link time. Certificate
   verification is always on (`verify_peer` + hostname check); a
   verify-disabled build is a security bug, not a convenience. Measured size:
   7.56 MB static, inside the ≤25 MB budget.


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


26. **Windows caps a path at MAX_PATH unless it carries the `\\?\` prefix.**
    `LongPathsEnabled` defaults to 0, so a deep cloned repository fails to
    resolve on a stock machine even though it works on a developer's. Every
    filesystem call in `workspace` therefore goes through
    `platform::to_extended_path` — including the `file_size` stat, which runs
    BEFORE the open and fails first. The prefix is applied at the call, never
    stored, so the paths the model and the logs see stay readable.


27. **A `docs/NN` citation in a code comment is banned, and prose around it
    breaks when the citation is removed.** Stripping `(docs/22 E4)` from
    `// ... the headless surface (docs/22 E4).` is safe; stripping
    `docs/22. A consumer script branches on these, so the` leaves a sentence
    with no subject. Read the whole comment, not the match.


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


45. **A streaming provider answers with `Transfer-Encoding: chunked`, and the
    framing must be removed before the event parser sees it.** The SSE body is
    read straight off the socket, so chunk-size lines and CRLFs reach the
    parser. A chunk-size line has no colon and is harmlessly discarded, but a
    chunk boundary landing inside an event splits its JSON across two lines —
    the continuation is discarded the same way and the truncated payload is
    rejected. Cloudflare fronts the gateway and always chunks, so this is the
    normal case. `net/http_internal.hxx` owns the decoder. A de-chunking
    loopback proxy hides the defect completely, which is how it shipped.


46. **OpenSSL on Windows has no CA file, and the trust store is loaded once.**
    `SSL_CTX_set_default_verify_paths` resolves to the *build machine's*
    OPENSSLDIR, which does not exist on a user's, so every handshake failed with
    `certificate verify failed`. The roots are read from the OS store
    (`CertOpenSystemStoreW(L"ROOT")` + `d2i_X509`). The resulting
    `ssl::context` is built once per process and deliberately never destroyed:
    a function-local static is torn down during static destruction, where its
    ordering against OpenSSL's own cleanup is unspecified, and the process
    aborted at exit whenever a TLS connection had been made.


47. **Multi-line explanatory comments are the established practice, against
    `AGENTS.md`'s "max one line".** The rule is worth keeping as the default —
    most comments should be one line or absent — but the tree already carries
    several hundred blocks that explain a non-obvious decision, and the merge
    batch added more. They are left in place deliberately: rewriting them to one
    line would delete the reasoning, which is the part a reader needs. New
    comments should still be one line unless the alternative is a reader getting
    the code wrong.


48. **An in-workspace write does not prompt, which is a deliberate deviation from
    `docs/12`'s table.** That table says `ask`; taken literally the agent prompts
    on every file edit, which is the "bothering the user" failure and enough on
    its own to make the tool unusable. An in-workspace write is already bounded by
    three things a prompt would not improve: the canonical-path boundary check,
    the read-before-write hash invariant, and the `.git/` / `.mcode/` deny. The
    prompt adds a keystroke, not a guarantee, and git is the real undo.
    `[permissions] ask = ["write"]` restores the strict reading verbatim — one
    config line, no code change, and both paths are tested.


49. **`--yolo` skips questions; it does not remove the floor.** A small hard-deny
    floor survives it: recursive delete of a filesystem root or the user's home,
    privilege escalation, `mkfs`, `dd` to a device, and partition tools, matched
    on the *canonical* target so `rm -rf /` and `rm -rf //` are one command. Every
    rule is tested against its legitimate near-miss (`rm -rf ./build`,
    `~/scratch/project-a`, `.gitignore`), because a false positive blocks real
    work and trains the user to disable the check. The floor is a separate check
    ahead of the rule merge, not a rule in the list: a rule can be overridden by a
    later scope and the floor must not be.


50. **`--yolo` now sandboxes where the platform can, and says exactly how much.**
    The OS sandbox exists: `sandbox_capability_level()` reports
    `write_boundary` on Windows (Low integrity token + mandatory labels via
    `CreateProcessAsUserW` — writes confined, reads not), `filesystem` on
    Linux (Landlock with runtime ABI detection), `filesystem` on macOS
    (Seatbelt, deny-default). The MCP server path gets the Job Object only.
    `--yolo`'s help text and `--verbose`'s sandbox line are asserted against
    the seam, so all three cannot drift. The floor above is still a policy
    check on parsed argv and canonical paths — it is the gate, the sandbox is
    the boundary, and neither claims the other's job.


51. **The remember store is `permissions.json`, not `config.toml`.** There is no
    TOML writer in the tree, and appending to a file the user hand-edits risks
    clobbering their comments and formatting. JSON is written by us, for us, with
    a sorted-key writer that already exists and is tested, and it is trivially
    inspectable and deletable. The user store and the project store are separate
    files: the repository's own `.mcode/permissions.json` is loaded read-only with
    every allow dropped, so a cloned repository can neither grant itself
    permissions nor rewrite the user's answers.


52. **A project-scope store cannot widen.** Enforced at load, with a warning, not
    at use — the same rule `config.cxx` already applies to project TOML. An
    `allow` from the project layer is dropped rather than honoured, because a
    one-clone compromise is the failure mode the rule exists to prevent.


53. **An extension tool needs a handler routed back into its VM.** A definition in
    the registry is not enough: the closure that runs it lives in the extension's
    VM, so `cli_commands.cxx` registers a handler per entry in
    `load_result::tool_owners`. Without it the model sees the tool in its schemas
    and every call returns "tool has no handler registered" — which is what
    happened the first time a bundled extension registered a *tool* rather than a
    provider.


54. **`docs/01` has no session-start budget row.** `AGENTS.md` points at `docs/01`
    for the budgets and lists a session-start row of ≤8.5K, but that row does not
    exist there; the figure and its breakdown live at `docs/05:71`, which marks
    itself the authority. This batch cites `docs/05:71` directly. The underlying
    inconsistency predates this batch and is not resolved here.


55. **A subsystem is not shipped until the host constructs it.** The MCP client
    was complete and fully tested — 16 cases against a fixture server covering
    restart, timeout, cancellation and hash-pinning — and **nothing in the
    binary ever built a `supervisor`**. A configured server never launched, and
    every test passed. The same shape as item 53: components that work, and a
    composition that does not exist. A test that drives a layer directly proves
    the layer; it says nothing about whether anything reaches it.


56. **`tool.pre_call` must be published by the loop, and its veto honoured.**
    The event and its `hooks.cxx` mapping existed, `bus::publish` already
    returned a veto, and nothing ever published it — so an extension hook that
    vetoes a tool call could never fire. It is published ahead of the handler
    and its veto denies the call, which is deliberately a **separate gate from
    the permission engine** and therefore not bypassable by `--yolo`.


57. **MCP servers come from two places that must produce one thing.**
    `[mcp.servers.<name>]` in `config.toml` is the primary path — no extension,
    no Lua. `mcode.mcp.register` is the extension-declared path. Both funnel
    into one `mcp::server_config` and one connect implementation, and a
    duplicate name between them is a hard error rather than a silent override.
    An extension-declared server still arrives `enabled = false`: declaring a
    server is not consenting to run it.


58. **The supervisors outlive the loop, deliberately.** They own the child
    processes, and the handlers the loop dispatches into reference them. The
    `server_set` is declared *before* the `agent_loop` in `run_exec`, so reverse
    destruction order tears the loop down first and no handler can outlive its
    target. Every early-return path has the same ordering.


59. **A test whose name contains a comma cannot be run by name.** 45 of the 698
    `TEST_CASE`s in this tree have a comma in their name — the repo's house style
    is `"a thing does X, not Y"` — and Catch2 treats the argument as a
    comma-separated filter list, so
    `mcode_tests.exe "a pending call is failed, not replayed"` matches nothing
    and exits 2, which reads as a failure. `ctest` is unaffected: it passes the
    name it registered. To run one by hand, use a wildcard —
    `mcode_tests.exe "*a pending call is failed*"`. This is not worth renaming
    45 tests for, but it costs a confusing ten minutes every time someone hits
    it, which is why it is here.


60. **A parallel build fails nondeterministically; use `-- -j 1`.** `cmake
    --build build/Release` fails 8 times out of 8 with
    `LNK1104: cannot open file '<a different>.obj'` — never the same file
    twice. It is a race between `lib.exe` reading the object list and other
    translation units still being written, not an ACL, Defender or handle
    leak: an exclusive-open probe on the named file succeeds, and
    `cmake --build build/Release -- -j 1` succeeds first try. Serial costs
    about 100 seconds on this tree, which is worth paying to avoid chasing a
    phantom permissions bug.


61. **A restricting SID cannot confine a child on Windows.** `CreateRestrictedToken`
    performs two access checks and the second consults *only* the restricting
    SIDs, so a synthetic sandbox SID — granted by no DLL — makes the child die
    at load with `STATUS_ACCESS_DENIED`. Measured, not theorised. Integrity
    levels confine **writes** without that second check, which is why the
    capability ladder has a `write_boundary` rung: Windows confines writes and
    not reads, and `sandbox_capability_level()` says so rather than claiming
    `filesystem`.


62. **`PROC_THREAD_ATTRIBUTE_TOKEN` does not exist.** The SDK's
    `PROC_THREAD_ATTRIBUTE_NUM` has no token attribute; the value 5 that an
    earlier revision of this tree used for one is
    `ProcThreadAttributeIdealProcessor`. A token can only be applied through
    `CreateProcessAsUserW`. The bogus constant meant the restricted token was
    silently never applied, so the sandbox reported a confinement that did not
    exist.


63. **A member function named after a type shadows that type inside the class.**
    `auto manifest() -> const manifest&` plus `manifest manifest_;` does not
    compile — the unqualified name resolves to the member function. Same
    failure with `session_state`. Qualify the type (`mcode::ext::manifest`) in
    both the accessor's return type and the member declaration.


64. **A designated initializer cannot skip a member and then name a later one.**
    `install_request{ .host = ..., .files = ... }` fails to compile when
    `.commands` sits between them in the declaration. Add the missing member to
    the initializer, or move it to the end of the struct.


65. **A pipe assigned to asio's IOCP service must be overlapped.** `CreatePipe`
    with a zero flag produces a synchronous handle, and
    `win_iocp_handle_service::assign` rejects it with `ERROR_INVALID_PARAMETER`
    (87) — reported as `assign: The parameter is incorrect`. Pass
    `FILE_FLAG_OVERLAPPED`. The boost launcher hides this because it creates its
    own pipes.


66. **Labelling a path that does not exist fails the whole spawn.**
    `SetNamedSecurityInfoW` returns `ERROR_FILE_NOT_FOUND` for an absent path,
    and the exec path adds `.git` and `.mcode` to the profile's deny list
    unconditionally — so `bash` failed outright in any workspace that is not a
    git repository. Skip a path that does not exist. The residual is recorded
    on `sandbox_profile::deny_paths`: a sandboxed child can create a
    not-yet-existing `.git`, which is narrower than refusing to run at all.


67. **A hand-copied mirror of a declaration file drifts, in both directions.**
    `test_api_surface.cxx` originally carried a 26-name array claiming to be
    "verbatim" from `extensions/mcode.d.luau`. It contained `cmd.handler` —
    which is a *parameter name* of `on`'s handler, not an entry point — and
    omitted `defer` and `notify`, both real. Two genuine gaps went undetected
    because the check compared a stale copy against the implementation. The
    test now parses the `.luau` at test time.


68. **The local gate compiles with `_WIN32` defined, so POSIX branches are never
    parsed on this machine.** `_clgate.py` catches the whole warning set for the
    Windows path and nothing at all for the others. There is no POSIX toolchain
    here — WSL2 is the bare `docker-desktop` distro — so Linux and macOS defects
    surface one CI cycle at a time, about 13 minutes each. Budget for it: this
    batch spent roughly fifteen cycles on errors that a single `g++` run would
    have listed at once. Writing the POSIX branch and believing it compiles is
    not a plan.


69. **`sandbox_init_with_parameters` takes the profile text only when flags are
    zero.** Passing `SANDBOX_NAMED_EXTERNAL` (0x0003) declares the first
    argument to be a *path to a profile file*, so the call tries to `open()` a
    kilobyte of policy text as a filename and fails with `ENAMETOOLONG`. Every
    macOS sandboxed spawn had been failing this way — Seatbelt had never applied
    to a single child — and the launcher reported only "File name too long".


70. **Seatbelt matches resolved paths, and `/dev` is a symlink to
    `/private/dev`.** `(subpath "/dev")` does not cover `/dev/null`, whose real
    path is `/private/dev/null`, so a shell could not open a redirect target and
    `bash` failed. `/etc` and `/tmp` have the same shape.


71. **A Landlock path-beneath rule cannot be added for a character device.**
    Granting write on `/dev/null` is refused, and the refusal fails the whole
    ruleset rather than that one rule — so every sandboxed command stopped
    spawning, not just the ones touching `/dev/null`. Read on `/dev` is fine and
    is what `grep x /dev/null` needs; write on a device has to be a different
    mechanism.


72. **A Landlock profile that grants only the workspace cannot execute
    anything.** `LANDLOCK_ACCESS_FS_EXECUTE` is handled, so an ungranted
    `/bin/sh` is denied and the spawn fails. The platform layer grants
    read+execute on the system roots (`/usr`, `/bin`, `/sbin`, `/lib`, `/lib64`,
    `/etc`, `/dev`) because that is what any process needs to load and run, and
    `exec_tools` should not have to know it.


73. **POSIX `exec` does not search `PATH`.** A bare `"sh"` fails with `ENOENT`
    even when a shell exists; Windows `CreateProcess` does search, which hides
    the difference on one platform. Resolve the executable before spawning.


74. **`apply_sandbox` restricts the calling process, irreversibly.** Neither
    Landlock nor Seatbelt can be undone, so a test that calls it in-process
    sandboxes its own runner and every later assertion fails on a denied
    `getcwd`. Enforcement belongs on the spawn path; `test_config_and_platform`
    asserts only the capability and mechanism.


75. **A quoted TOML key segment is taken verbatim, so `[models."<id>"]` needs
    no escaping.** The id is written literally, including its `/` and `.`, and
    the entry flattens to `models.<id>` exactly. Quoting is required rather than
    cosmetic: a bare key rejects `/`, and mangling the id instead (`_` for `.`)
    would let two ids collide and silently price one at the other's rate.


76. **A gateway may report the whole prompt as a cache read, even on the first
    request.** InferHub does: `cached_read` equals the full input count, so
    `billed_input` in `compute_cost` is zero and the input contributes nothing.
    A `price_cached_read` of 0 is therefore not "unknown" but the claim that
    input is free, under-estimating a run by its whole input cost — and a budget
    that under-estimates stops late rather than early. `resolve_capabilities`
    falls back to `price_input` when the config sets no cached-read price, so
    the default assumes no discount instead of a total one.


77. **`read_key` returning `exit` on a timeout made an idle prompt quit.**
    `tty_session::read_key` used `exit` for both "the user pressed Ctrl+D" and
    "the wait elapsed with no key", so a session ended itself whenever the user
    paused. `key_event::kind::timeout` now carries the second case and the loop
    repaints and keeps waiting. The same conflation was on both platform
    branches, Windows and POSIX.


78. **The first rendered frame was swallowed.** `render_coordinator::flush`
    returned an empty string for the first frame, recording it as the diff
    baseline without drawing it. The frame builder was only ever reached from a
    bus event, so at an idle prompt nothing appeared until the user typed --
    no prompt, no status line -- which reads as a program that hung or exited.
    `resize` has already blanked `previous_` to the right size, so the first
    frame can be emitted against it directly.


79. **`sgr` opened and closed the sequence before appending parameters.** It
    built `"\x1b[0m"` — a complete reset — and then appended `;90;37` and `m`
    outside the bracket, so the terminal executed the reset and printed
    `;90;37m` on screen as literal text. Every frame was affected, and the
    bytes still parsed as valid ANSI, which is why only reading the output
    catches it. `test_tui_frame` now walks the emitted bytes and fails on a
    parameter character outside a sequence.


80. **The console decodes written bytes with its own output codepage.** A frame
    carries UTF-8 (`▸`, the spinner, box drawing) and the session writes raw
    bytes, so under the default OEM page one separator rendered as `Γû╸`.
    `tty_session` now sets `CP_UTF8` for the session and restores the previous
    page on the way out, alongside the console modes.


81. **`turn_end` must fire on every exit path.** It sat after `run_state_machine`'s
    loop, but `handoff`, `done` and `failed` all `return` from inside that loop,
    so it was never published at all. The TUI commits a turn's answer on
    `turn_end`, so every reply was cleared with the live region and never
    reached the transcript -- the session looked like it had answered and then
    forgotten. It is now a destructor guard, which cannot be skipped.


82. **The live region addresses relatively, from a parked cursor.** Committing
    scrolls the region clear and then re-parks; nothing may address an absolute
    row, because a committed line changes how many rows are above the region.
    One column is reserved in `resize` so a full row cannot wrap and desync the
    relative addressing, and `commit` scrolls by the region's full height
    whatever the text's length -- scrolling by the text's own line count left
    the first committed lines exactly where the region redraws.


83. **`ReadConsoleInputA` consumes every record it reports.** The decoder
    returned on the first key and dropped the rest of the batch, so typing
    faster than the poll interval lost keystrokes, and a split escape sequence
    could end the session. It now decodes the whole batch into
    `tty_session::pending_`, and honours `wRepeatCount` so a held key repeats.


84. **The POSIX key reader must buffer partial sequences.** It compared each
    read against whole escape strings and fell through to "control byte" --
    which is `exit` -- so a paste or a split arrow key ended the session. The
    tail now stays in `carry_` until a complete sequence or UTF-8 character
    arrives.


85. **A `tui::token` with no theme entry renders in the wrong colour, silently.**
    `entry_index` falls back to index 0, so a token added to the enum and left
    out of `ENTRIES` gets `token::none`'s value instead of failing. That is how
    `token::thinking` rendered as plain default text. The table's size is now
    inferred (`std::to_array`) and checked against `TOKEN_COUNT`, so an omission
    is a compile error rather than a colour nobody notices.


86. **Anything the tests exercise must live in `mcode_core`, not the `mcode`
    executable.** `mcode_tests` links `mcode_core` only, so a module compiled
    solely into the executable is untestable -- the test target fails to link
    with unresolved externals. `cli/slash.cxx` was moved into the library for
    exactly this reason; `cli_repl.cxx` stays out because nothing links it.


87. **`token::none` is the absent sentinel, and resolving it to a colour made
    the whole UI monochrome.** It is the default `background` of every span, and
    `sgr` appends the background unconditionally, so a resolved value emitted a
    *second foreground* after the real one. A terminal applies the last, so
    every span rendered in `none`'s colour and the theme was effectively unused.
    `token_color` now returns empty for it. Any token used as a background needs
    a `48;`-family value, never a `38;` one.


88. **The console INPUT codepage must be set to UTF-8 alongside the output one.**
    The output side was set to `CP_UTF8` and restored on exit, but input was
    left at the process default, so a non-ASCII keystroke or paste arrived as
    legacy-codepage bytes, decoded as invalid UTF-8, and rendered as a gap in
    the prompt echo. `SetConsoleCP`/`GetConsoleCP` now mirror the output pair.


89. **A raw-mode line reader must echo, and must never re-prompt silently.**
    `ENABLE_ECHO_INPUT` is off (deliberately: the console is read with
    `ReadConsoleInputA`), so nothing the user types reaches the screen unless
    the reader writes it back -- a prompt that takes input but shows none reads
    as a hang. The same failure came from the other side: an unrecognised answer
    re-read without repainting, so `yes` (rather than the single letter `y`)
    looped forever with a byte-identical screen. The prompt now echoes what it
    consumes, accepts the long spellings, and always shows a rejection or a
    result.


90. **A streamed answer was rendered one line deep and committed raw.** The
    live region kept `streaming_line = render_inline( tail_of( text ) )`, so
    every line but the last was invisible until the turn ended; at commit the
    same text was pushed as one span with its newlines and markdown markers
    intact. Both ends now go through `render_markdown`: the whole buffer is
    re-rendered into `streaming_rows` (and `thinking_rows`) at most once per
    paint -- a delta only appends and marks the rows stale -- the region counts
    those rows so it grows with the block, and the commit renders whatever the
    buffer holds before it pushes it. A block taller than the 16-row cap keeps
    its newest rows, because the frame is filled bottom-up and the earliest rows
    are the ones that fall off. The reasoning commits as a bounded block of
    `THOUGHT_COMMIT_MAX_ROWS` rows under the `✻` gutter rather than a truncated
    first line. The queueing and commit protocol moved to `transcript.cxx`,
    which keeps `render.cxx` under the 600-line cap.


91. **A raw-mode newline moves down without returning the carriage.** `commit`
    separated committed rows with a bare `\n`. POSIX raw mode clears `OPOST`,
    so the cursor kept the previous row's end column and every row of a
    multi-row block started further right than the one above it -- the
    stair-step was invisible while the commit was a single span with embedded
    newlines, and obvious once it carried a rendered block. Windows keeps
    `ENABLE_PROCESSED_OUTPUT`, which translates the newline, so the artifact was
    POSIX-only. Committed rows now end with `\r\n`.


92. **The streamed block's rows are materialised on read, never on the delta.**
    Re-rendering the whole accumulated buffer per delta is quadratic in the
    answer's length: measured over a 24 KB markdown answer fed one byte at a
    time, 24089 deltas cost 24089 full renders and ~3.2 s of render work, on the
    same thread the agent runs on. A delta now only appends to
    `streaming_text`/`thinking_text` and sets a stale flag, and the rows are
    re-rendered by the next reader -- `state()`, `region_rows_for` (which sizes
    the region before the frame is built) and `build_frame` -- so the
    whole-buffer render happens at most once per paint. The commit paths in
    `apply` (`tool_start`, `turn_end`) refresh before they read the rows, so a
    block is always committed from the text it holds. `transcript::render_block`
    is still the only producer of rows: the deferral is a cache with one
    materialisation site, not a second renderer. `stream_render_count()` makes
    the bound assertable.


93. **A commit must repaint the region in the same flush.** `flush` returned
    `commit(...)` alone, and `commit` erases the region to print above it -- so
    the screen sat with no prompt row and no status line until the next event
    arrived. Committing several blocks in a burst (a tool call per delta) left
    the region blank for most of a turn: sampled at 50 ms over one two-file read
    turn, 204 of 1200 frames had no prompt row. The commit is now prepended to
    the paint instead of replacing it, and the same sampling shows 0 of 1400.


94. **A committed row must wrap with a hanging indent.** `cell_buffer` clips at
    the row edge by design (the live region is a fixed grid), so a committed row
    longer than the terminal was cut by the terminal itself and continued at
    column 0 -- a wrapped bullet lost its indent and read as a new top-level
    line. `transcript::wrap_rows` now wraps at word boundaries, on grapheme
    clusters, carrying the row's own leading indent onto every continuation row
    and keeping each span's colour, background and attributes across the break.
    Fenced-code rows are passed through verbatim: their `token::code_bg`
    background is the only surviving signal that they came from a fence.


95. **Plan and Act were two full stateless requests, so a greeting paid the
    whole prefix twice.** Every request carries the system prompt, the
    instruction chain and the tool schemas; the loop issued one for Plan and
    another for Act, and a turn that needed no tool produced the same answer
    from both. Plan now owns the turn's first request, and when its response
    carries no tool call that response *is* the answer: Act reuses it and the
    run goes straight to Verify. When the response does carry tool calls they
    are dispatched by Act as before, so no tool call is ever skipped. Act
    dispatches its pending calls *before* the step-budget check, so a request
    that spends the last step still executes what the model asked for.


96. **East-Asian *ambiguous* width is a real terminal hazard, and it is now
    resolved by a probe.** The chrome glyphs `│` U+2502, `─` U+2500, `•` U+2022
    and `…` U+2026 render as one column or two depending on the terminal's East
    Asian Width setting. `probe_capabilities` reads `MCODE_AMBIGUOUS_WIDTH` once
    at startup into `capabilities::ambiguous_width`, clamped to exactly {1, 2}:
    `2` means the terminal paints those glyphs two columns wide, and unset, `1`
    or any other value means one (an unknown value is not an error and is not
    logged). Every measurement — the frame builder, the commit wrap and the
    caret column — reads that one probed value, so a terminal that disagrees
    with the default can no longer desynchronise the cursor from the grid and
    splice a phantom space into neighbouring text. The variable is the whole
    interface; there is no config-file key.


97. **The retained scrollback is not the terminal's scrollback.** Committed rows
    still print through the normal commit path and remain in the terminal's own
    scrollback, which is the durable transcript. The coordinator additionally
    keeps a bounded copy (`SCROLLBACK_MAX_ROWS`) so PageUp/PageDown and the
    wheel can repaint history inside the live region without moving the
    terminal's viewport. Scrolling never writes to the terminal at all: it
    changes an offset and lets the cell diff repaint, and no destructive clear
    is ever emitted (`clear_region` uses `CSI 2K` per row; there is no `CSI 2J`,
    `3J`, `S` or `T` anywhere), so a scroll can never destroy the real
    transcript. Mouse reporting is off by default for the same reason: a
    terminal reporting the wheel stops scrolling its own scrollback, which is
    how history is read, so reporting is enabled only while a turn runs. The
    offset is clamped to the deepest one that still fills the viewport's body
    (`retained - (region rows - 1)`, floored at `1` while more than one row is
    retained): a deeper offset would paint blank rows above the oldest retained
    row instead of history, which is what made a PageUp look empty. The clamp
    is re-applied on resize, because the viewport's height is the screen's, but
    not on commit, where `retain()` instead advances the offset by the count it
    appends so the visible rows stay put.


98. **Esc is a cooperative interrupt, because there is no cancellation seam.**
    `model_client::stream` takes no cancel token, `http_client::stream_sse` has
    no cancel path and the loop has no `request_cancel`, so Esc cannot abort a
    request in flight. It sets a flag that the event-bus subscriber reads to
    stop consuming further deltas; the turn then finishes cleanly on what
    arrived, partial text stays in the transcript, `interrupted` is committed as
    a warning line and the process exits 130. A `tool_pre_call` veto and a
    step-budget clamp were both rejected: the first would log a denial as a
    permission decision, the second would make an interrupt indistinguishable
    from budget exhaustion.

99. **LuaJIT's Makefile refuses to build on Darwin without
    `MACOSX_DEPLOYMENT_TARGET`.** It must be *exported*, not passed as a make
    variable: the check reads the environment. The error is
    `*** missing: export MACOSX_DEPLOYMENT_TARGET=XX.YY`.


100. **`conan/profiles/windows-msvc` targets MSVC 194, not this machine's 195.**
    A bare `compiler.version` is read as a *Visual Studio generation* number, so
    195 means "Visual Studio 18" and the recipe generator fails with
    `VS non-existing installation: Visual Studio 18` on any runner without it.
    The profile carries the portable value plus
    `tools.microsoft.msbuild:vs_version=17`; override on the command line for a
    local VS 18 toolset.


101. **Conan's Apple Silicon arch is `armv8`, not `arm64`.** `arch=arm64` fails
    with `Invalid setting 'arm64' is not a valid 'settings.arch' value`.


102. **GCC's `-Wshadow` and `-Wmissing-field-initializers` fire under
    `-DMCODE_WARNINGS_AS_ERRORS=ON` and MSVC does not have them.** A designated
    initializer that leaves a field defaulted, and a local shadowing a member,
    both compile clean on MSVC and fail the Linux leg. The Linux job is the only
    thing that catches these.


103. **A designated initializer that leaves a field defaulted fails the GCC and
    Clang legs.** `-Wmissing-field-initializers` is on under
    `-DMCODE_WARNINGS_AS_ERRORS=ON`, and MSVC has no equivalent. Construct the
    struct and assign the fields, or initialize every member.

104. **A tool call's arguments are repaired before they are validated, and
    validation happens in `agent_loop::execute`, not in the handler wrapper.**
    The wrapper runs for core tools only; `execute` is the one path every tool
    takes — core, extension and MCP alike. Validating in the wrapper would leave
    extension and MCP calls unchecked. The repair pass is inside
    `tool_args::parse` so no caller can forget it.

105. **A truncated response is a distinct failure from a malformed one.** The
    provider's finish reason (`length`, `max_tokens`) is read from the stream and
    recorded on each call. Without it, a call cut off mid-JSON is reported as a
    parse error and the model is told to fix a parameter that was never wrong.
    `tool_call::truncated` carries the flag; `tools::truncation_error` renders it.

106. **The tool-call index on the wire is unreliable.** It is optional, and a
    gateway may pin it at a constant (ollama shipped both bugs). Fragments are
    keyed by an ordinal the applier assigns; the wire index only *selects* a
    candidate, and a fragment that contradicts its candidate starts a new call.
    Keying by the index directly merges distinct calls into one.

107. **`strict` and `parallel_tool_calls` are two independent flags, and the
    model's capability table is not enough.** `supports_strict_schema` says the
    model honours the grammar; `features.strict_tools` says the gateway accepts
    the field. Both must be set or the request is rejected outright.

108. **A 400 that names a request field is not a malformed request.** The gateway
    is refusing an optimisation it does not implement. The field is dropped and
    the request retried, bounded by `MAX_FEATURE_DOWNGRADES`, and the retry does
    not consume an attempt — the request that failed was never servable. A 400
    that names no known field stays fatal.

109. **A JSON response format is dropped when a tool list is present.** Some
    open-weight models stop calling tools when both are set. Tool-calling is the
    primary mode, so the format yields unless the descriptor declares the gateway
    honours both.

110. **The cache needs a rolling second breakpoint.** A cache read walks back
    only a bounded number of blocks from a breakpoint, so a single anchor on the
    stable prefix stops hitting once the history in front of it is long enough.
    `cache_breakpoints` returns the stable-prefix offset *and* one on the newest
    message. Breakpoints are applied right-to-left, because a marker occupies
    bytes and would otherwise shift the next offset.

111. **There is no stream stall watchdog, deliberately.** The transport sets a
    per-read timeout (60 s) and nothing shorter: Anthropic streams tool input as
    partial JSON one key at a time with multi-second gaps, so an inactivity
    detector would abort a healthy stream. Do not add one.

112. **`--plan` is checked ahead of the floor and ahead of `--yolo`,** in the
    same place the floor is. A read-only mode that a permissive flag can escape
    is not read-only. `tool_class::mcp` is denied too: an MCP `readOnlyHint` is
    an untrusted claim, not a read class.

113. **The approval default is permissive for an interactive session and
    conservative for a headless one.** The interactive default is `never`, which
    is safe only because the hard-deny floor and every `permissions.deny` rule
    are decided *ahead* of the approval mode, and because a human is present to
    read the boundary printed on entry and to use `/undo`. A headless run has
    neither, so it keeps the engine's own `on-request` default and fails closed.
    The engine default is deliberately conservative: a caller that sets nothing
    must not auto-allow. `--ask` restores prompting and wins over `--yolo`.

114. **The floor resolves a target before matching it.** `weakly_canonical` alone
    leaves a final symlink or junction in place, so `rm -rf link-to-root` would
    name a path that is not a root. The final component is resolved with
    `std::filesystem::canonical`, falling back to the parent-resolved form when
    the path does not exist (a path that cannot be resolved cannot be deleted).

115. **The audit trail logs the expanded command, not just its output.** A
    permission incident is otherwise uninverifiable: the recorded output says what
    happened, not what ran. `tool.command` is appended after the permission check
    so it reflects what was approved.

116. **A child process starts in `process_options::working_directory`, and that is
    passed explicitly on every spawn path.** Boost.Process inherits the parent's
    directory unless given a start directory, so an unsandboxed spawn silently ran
    in the harness's own directory rather than the workspace root — invisible while
    the two coincided, wrong the moment they did not. The POSIX sandbox launcher
    dropped the field entirely.

117. **`--worktree` refuses a workspace that is not the repository root.** `git
    worktree add` walks up to an enclosing repository, so a plain directory inside
    one would check out the *wrong project* and appear to succeed. The root is
    confirmed against `git rev-parse --show-toplevel` before anything is created.

118. **The live region's painted height is tracked, never assumed.** The height is
    dynamic — prompt + status at the floor, plus palette, thought and tool rows —
    so the coordinator stores what is actually on screen (`painted_rows_`) and
    starts it at the floor. A larger initial value made `commit` erase and scroll
    rows the region had never covered, and the first screen opened with a five-row
    gap under the header. **Nothing may scroll the screen after a commit.**
    `commit` already scrolls what it wrote clear of the region; any further scroll
    pushes the committed block back off the top, which is how the startup banner
    vanished entirely while the code looked correct. The `reserve()` that emitted
    `screen_rows - 1` newlines is gone for that reason: the region is positioned
    absolutely by `park`, so it needs no scroll to sit at the bottom, and the
    cursor is already on the last row when `reserve` would have run. A caller that
    must erase the region after the last flush asks for `painted_height()` rather
    than assuming the floor.

119. **Landlock cannot express a deny path, so the Linux sandbox ignores
    `sandbox_profile::deny_paths`.** Landlock is an allowlist: a rule grants
    access beneath a path, and a more specific rule only ever *adds*. The
    workspace root is granted read-write, so `.git/hooks` and `.mcode/` beneath
    it stay writable by a spawned command even though the Windows backend marks
    both MEDIUM_IL to block a Low-IL child. The profile still carries the field
    and the Windows backend still honours it, so this is a **platform gap, not a
    bug in the shared type** — but it is real, and it is stated rather than
    assumed. Closing it means granting the workspace's children individually
    instead of the root, which is a behavioural change that needs a Linux host to
    verify. **Nothing on Windows or macOS depends on it.**

120. **A tool result must be logged under `TOOL_RESULT_EVENT` and carry its call
    id.** `restore_transcript` reads `tool.output` records and matches each to
    the call it answers. A failure was logged under the literal `"tool.result"`
    — the *event name*, not the log kind — so it never matched, and a resumed
    session silently lost every failed call's result. Without the id, one
    assistant message carrying two calls got two results stamped with call #1
    and left call #2 unanswered, which is a transcript no provider accepts.

121. **`std::regex` cannot be time-bounded, so a grep pattern is refused before it
    runs.** The line cap does not bound backtracking: a nested quantifier such as
    `(a+)+` is exponential in the line length, measured at 13 s inside one
    `regex_search` on a 26-character line, and the matcher offers no way to
    interrupt it. `has_nested_quantifier` rejects the exponential shapes up
    front, and a wall-clock budget bounds the scan between lines. The refusal is
    honest — a silent 13-second stall reads as a hang — and the hint says the
    scan is incomplete, so a short result is never taken as proof of absence.

122. **A `\\?\` extended path cannot contain a forward slash.** Windows rejects
    `\\?\C:/...` outright, and `std::filesystem::path` preserves the separator
    it was given, so `root / "src/page.html"` is mixed and the prefixed result
    does not resolve. `to_extended_path` now normalizes separators before
    prefixing. The failure was invisible: `grep` scoped to a directory opened no
    files, scanned zero, and returned `{"ok":true,"matches":[],"count":0}` — a
    well-formed empty result a reader cannot tell apart from "no matches". Any
    seam that prefixes a caller's path must normalize it first.

123. **A negative exit code is an NTSTATUS, not a return value.** `0xC0000142`
    means the process never started, which is a different problem from a command
    that ran and failed, and the natural response to the wrong diagnosis is to
    retry the same binary. `bash` decodes the common ones and says which it is.
    A bare `exit code -1073741502` in a tool result tells a model nothing.

124. **`ok` in a tool result means "the tool ran", not "the command succeeded".**
    A `bash` call whose process crashed still produces `ok: true`, because the
    handler returned normally. A consumer that reads `ok` as success is wrong,
    and an extension cannot condition on failure at all — there is no failure
    signal at that layer. The payload carries `succeeded` for the command's own
    outcome; `ok` is about the dispatch.

125. **MSYS binaries fail under the Windows sandbox.** Git's `grep`, `rm` and
    `sed` create a named object under `\BaseNamedObjects`, which the low-integrity
    token denies, so they die with `NtCreateDirectoryObject(...) 0xC0000022` before
    doing any work. Environmental, not a harness defect — but it is why the
    dedicated `grep`, `glob` and `read` tools matter, and why a `bash` call to
    coreutils is not a substitute for them on this platform.
126. **A gate that reads the developer's config is not a gate.** `tui-screen`
    spawns the real binary under a ConPTY and asserts on the screen it draws, but
    it inherited `%APPDATA%` -- so on any machine that had not been configured,
    `mcode` exited with "no provider configured" before drawing a frame. It
    passed on every developer machine and failed on every clean CI runner, and
    the empty screen it produced was indistinguishable from a rendering defect:
    the report said `the prompt is the last row and bare -- ''`, which reads as
    a TUI bug rather than a missing config file. The gate now writes its own
    config into a private `APPDATA` and `LOCALAPPDATA` and sets the credential
    variable its fixture names. Any gate that spawns the product must own every
    piece of state the product reads.
127. **A tool that echoes captured terminal content must force UTF-8 stdout.** The
    `tui-screen` gate quotes the screen it read, and the meter contains `▸`
    (U+25B8). A CI runner's stdout defaults to a legacy code page — cp1252 on
    Windows — so `print` raised `UnicodeEncodeError: 'charmap' codec can't encode
    character '\u25b8'` and the gate failed *while reporting its verdict*, with a
    traceback where the verdict should have been. Every startup assertion had
    passed. `sys.stdout.reconfigure(encoding="utf-8")` at the top of the tool is
    the fix; `PYTHONIOENCODING` alone is not enough, because the environment that
    sets it is the same one that gets it wrong.
128. **A `TEST_CASE` name must begin with an alphanumeric, or it is unrunnable on
    Windows only.** `catch_discover_tests` registers each test by name and
    `ctest` passes that name back as a filter argument. Clara -- Catch2's CLI
    parser -- accepts `/option` as an option on Windows and rejects it
    everywhere else, so a name beginning with `/` makes the binary fail with
    `Unrecognised token: /extensions` and the test fails. Linux and macOS run the
    same name without complaint, so this is a green-on-Linux, red-on-Windows
    failure that costs a CI cycle to find. Measured: five tests named
    `/extensions ...` and `/init ...` passed the GCC and Apple Clang legs and
    failed all five on MSVC. Names beginning with `[` are unaffected, because
    Catch2 parses a leading bracket group as a tag. Rename to start with a word --
    "the extensions command reports ...".
129. **A Windows integrity label propagates to everything beneath the path, so
    labelling a directory is not a constant-cost operation.** The sandbox grants
    the child a writable temp by setting `LABEL_SECURITY_INFORMATION` with
    `SetNamedSecurityInfoW`. Granting the user's `%TEMP%` therefore walked and
    relabelled their entire temp tree -- and left it that way -- on **every**
    `bash` call. Measured: 20,770 ms of a 21,015 ms spawn, on a real run where 13
    bash calls took 238.8 s of a 437 s session (mean 18.4 s each) for commands
    that run in milliseconds. After giving the child a directory of our own under
    the system temp, named with the process id and created once per process: 1 ms
    and 23 ms end to end. Two lessons. The cost is in the *grant*, not the spawn,
    so instrument the phases before assuming process startup is slow. And a
    sandbox grant must be as narrow as the work needs, because on Windows its
    price scales with the size of what you granted.

130. **Backslash escapes inside double quotes are not optional to model, and not
    sh's either.** `parse_command_line` scanned quoted text for the closing quote
    without honouring `\"`, so `node -e "...||..."` ended the string early and
    the `||` was read as an *unquoted* pipe: the gate refused a command as
    "compound" when nothing about it was, and the model lost a turn to it. The
    fix is not "consume every backslash" -- that turns a quoted `"C:\Users\x"`
    into `C:Usersx`. Only the characters sh escapes inside double quotes
    (`"` `\` `$` `` ` `` and newline) are consumed; every other backslash is
    literal, which is what keeps a Windows path intact. Single quotes take no
    escapes at all, as in sh.
131. **A version bump is not visible until CMake reconfigures.** `MCODE_VERSION`
    reaches CMake from the Conan toolchain, which sets it with
    `set(... CACHE STRING ...)` and no `FORCE`. A cache entry therefore wins over
    the regenerated toolchain, so editing `conanfile.py` and running
    `conan install` is not enough: the binary keeps reporting the old version
    until the cache entry is removed or the configure is forced. Cost two full
    rebuild cycles to notice, because `--version` printed the previous number
    while the toolchain file on disk already said the new one. CI is unaffected --
    a clean runner has no cache to win -- which is exactly why it is invisible
    locally and worth writing down.


