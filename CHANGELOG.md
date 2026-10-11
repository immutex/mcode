# Changelog

Notable changes to mcode. This file follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Entries are one line per change, under the type of change: **Added**, **Changed**,
**Fixed**, **Removed**, **Security**, **Documentation**.

## [Unreleased]

## [0.0.3] - 2026-10-10

The fourth batch. Three of the harness's four differentiators were documentation
rather than code: a repo-local extension trust gate that did not exist and was
provably exploitable, an egress proxy `docs/12` called its highest-value control
and had never been built, and an `mcode ext install` that `docs/25` specified and
no command implemented. This release makes those three claims true, wires a
verification gate that existed with zero callers, fixes a context meter that
reported cumulative spend as window occupancy, and adds memory, subagents and MCP
over HTTP. No new dependency in any of it.

### Added

- **Extension trust gate.** A repository's `.mcode/extensions/` is now inert until a grant matches its tree digest; before this, a cloned repository's `init.luau` ran on session start with no prompt.
- `mcode ext list`, reporting each discovered extension's scope, trust state and version.
- `mcode ext trust <name>` and `mcode ext deny <name>`, taking a name from the discovered set rather than a path, so there is no path argument to race.
- `mcode ext install git:owner/repo@<ref>`, resolving the peeled commit before fetching, refusing submodules, and recording the resolved SHA in a lockfile.
- The lockfile records `commit` and `tree_sha256` both, because an EOL filter makes the working tree differ from the committed blob while the commit ID stays valid.
- `text::count_invisible`, counting format and control codepoints and variation selectors, reported when granting or installing so a payload hidden in an invisible Unicode range is visible.
- **Egress policy and proxy.** A loopback HTTP `CONNECT` and SOCKS5 proxy enforcing a host allowlist, with resolved addresses checked so an allowlisted name cannot be aimed at loopback or a cloud metadata endpoint.
- `sandbox_network_support::mediated`, distinct from `enforced`, so Windows reports `best-effort` rather than claiming parity it does not have.
- **Plan-then-execute taint gate.** An MCP tool result marks the session, and a net or out-of-workspace action then needs confirmation regardless of rules and regardless of `--yolo`.
- **Verification gate.** `--verify <command>` and `[verify] command` arm the check the loop already implemented; `set_verification_command` had zero callers before this, so no run was ever verified.
- `verification_verdict` (`no-tests`, `verified`, `failed`), carried on the run outcome, in the `run.end` log record, in `exec --json` and in the human summary.
- `false_success_rate`, computed from session logs and printed by `mcode eval`, as the trustworthiness number the reliability doc argues for.
- **Context controller.** Tool results are cleared before the history is compacted, which is the cheaper lever and the one that does most of the work.
- `context_occupancy`, tracking the last request's prompt separately from the run's cumulative spend.
- `/context` reports window fill, the cache hit ratio and the rewrite count, rather than one ratio against a number that grew without bound.
- **Memory.** A user-authored notes ledger, `/remember <text>` to append to it, and a `memory_search` extension that greps it; the ledger lives outside the workspace and no model writes to it.
- **MCP over Streamable HTTP**, so hosted MCP servers are reachable; the transport interface now has two implementations and the supervisor is transport-agnostic.
- **Read-only subagents** in `agent/subagent.hxx`, restricted by building the child's registry from the read class, so a write call fails schema validation rather than reaching an approval prompt.

### Changed

- `context_used()` now returns window occupancy from the last request; it returned the budget's cumulative counter, which the TUI meter and `/context` divided by the context window, so a long session read over 100%.
- `tokens_spent()` added for the cumulative figure, which is what the budget is charged on.
- Compaction thresholds moved to `agent/context_controller.hxx`, where the policy lives.
- `load_manifest` split into a file reader and `parse_manifest`, so the loader and the installer validate with the same function and cannot drift.
- `lua_host::run_file` removed; the loader serves the entry point from the byte snapshot it hashed, so the bytes checked are the bytes compiled.
- `http_client` is movable, so a transport that owns one can be returned by value.
- `sandbox_profile` carries an `egress_endpoint`, and macOS permits only the proxy port rather than the whole network when one is set.
- `supervisor` holds a `transport`, not a `stdio_transport`, so the HTTP path needs no second supervisor.
- `mcp` server config accepts `transport = "http"` with a `url`, and validates that one or the other is present.

### Fixed

- **Anchored `git ls-remote` resolution.** An annotated tag emits two lines and only the `^{}` line is the commit; recording the first line pinned the tag object instead.
- A `python -c` in a read-only subagent cannot write files, because the child has no shell tool at all.
- Clearing tool results keeps the assistant call that produced each one, so the model can still see what it asked for and re-issue the read.
- `egress_policy` denies by default and refuses a bare `*`, so a configuration mistake cannot become a network grant.

### Removed

- `TOOL_CLEAR_TRIGGER_FRACTION` and `CLEAR_AT_LEAST_FRACTION` from `agent/loop.hxx`; declared, read nowhere, and now owned by the controller.
- `MAX_POST_COMPACT_REREADS` and `POST_COMPACT_REREAD_TOKENS`, which no code ever read.

### Security

- **A cloned repository can no longer execute code.** A project extension with no grant is read, hashed and reported, and never executed.
- **A changed extension is not re-granted.** A digest mismatch makes it inert and names the difference; nothing rewrites the grant.
- The trust store is line-oriented and fails closed: one unparsable line fails the whole store rather than being skipped, and an unknown format version is a hard error.
- `mcode ext install` clones with hooks disabled, `protocol.file.allow=never`, and no submodules, and refuses a repository declaring them.
- Direct egress from a sandboxed child is refused on Linux and macOS by the OS rule; `HTTP_PROXY` is a hint, not the boundary, and the docs now say so.

### Documentation

- `docs/43-fourth-batch.md` is the plan this release implements, with the mechanism, files, seam and honest cost for each decision.
- `docs/12` records that the egress paragraph overstated its case: an allowlist raises the cost of exfiltration, it does not stop it through an allowed destination, and it does not break the lethal trifecta.
- `docs/09` carries a banner superseding its FTS5 design, which contradicted `01`, `03`, `14` and its own Traps section.
- `docs/10` rewritten: read-only subagents ship, parallel writers do not.
- `docs/05` and `docs/11` marked as specifications rather than descriptions, with the constants that were dead and the gate that could not be armed named explicitly.
- `docs/25` and `docs/22` updated for the install command and the new CLI surface.
- `docs/16` corrected where it stated the superseded memory and grant designs.
- `docs/00-index.md` registers the batch and warns that `12` and `25` describe design until it lands.

## [0.0.2] - 2026-10-09

The second release. Almost everything here came from running the harness on real
tasks and reading the resulting session logs rather than from reading the code,
which is why the fixed list is long: a green build and a passing suite do not
notice a log that cannot be parsed, a permission gate that refuses valid
commands, or a 20-second spawn.

### Added

- `/resume`, `/continue` and `/new` slash commands, so a session can be switched without exiting and restarting.
- `/resume` picker listing each session with its start time, log size and opening request, rather than an opaque id.
- `/init` now generates `AGENTS.md` from the repository by submitting a turn, instead of writing a scaffold with empty sections.
- `/extensions` command reporting per-extension version, tools and VM memory, then every load failure with the loader's own reason.
- `deepseek-guard` bundled extension, detecting DeepSeek tool-call markup leaking into assistant text.
- `assistant.thinking` is now exposed to extensions; the bus had the kind and the hook table did not.
- `Ctrl+C` interrupts a running turn, as the session banner already claimed; both it and `Esc` set a cancel source the loop reads at a step boundary.
- Repetition guard breaks a thinking loop instead of ending the run, re-asking with an action-oriented steer at most twice.
- Session opening header, committed to scrollback, carrying the version, platform and approval boundary that previously scrolled away on stderr.
- `Waiting` status between submitting and the first delta, the gap every content-derived signal left silent.
- A tool result that fails now records its reason, failure class and elapsed time instead of a bare `{"ok":false}`.
- `bash` results carry `succeeded` for the command's own outcome, distinct from `ok` for whether the tool ran.
- Budget stops name the limit that bound (`step budget exhausted (1 of 1 steps)`) instead of a generic `budget exhausted`.
- The session log's `run`, `turn` and `step` envelope fields are now written; they were declared and never assigned.
- A completed run writes the same rich `run.end` summary a failed one does, including tokens, cost and step count.
- `tools::loop_failure` and `tools::loop_denial`, one builder for the failure envelopes previously hand-built in eleven places.
- `path_is_within`, one implementation of the workspace containment walk previously copied four times.
- `argument_error_text`, so the plain and TUI entry points report a bad argument identically.
- `platform::sandbox_temp_directory`, a run-scoped writable temp instead of the user's whole `%TEMP%`.
- `text::ascii_lower`, `text::program_basename`, `text::human_bytes` and `text::CHARS_PER_TOKEN`, each replacing several drifting copies.
- `search_walk.{hxx,cxx}`, separating the workspace walk and regex guard from the two search handlers.
- `cli/init_prompt.hxx`, moving the 139-line `/init` prompt out of the command surface as data rather than logic.
- `tools/tui_check/tui_screen.py`, a ConPTY gate that drives the real binary and asserts the rendered screen.
- `tui_test::screen_model`, a terminal in miniature, closing the class of defect where a scrolled-off header and a kept one render identically.
- `tools/bench` budget gate and `tools/spike` VM conformance probes wired into CI.
- `LICENSE` (Apache-2.0), `CONTRIBUTING.md`, `SECURITY.md` and `CODE_OF_CONDUCT.md`.
- `.github/dependabot.yml`, issue forms, a pull request template and `CODEOWNERS`.
- Repository description, topics, private vulnerability reporting and Discussions.

### Changed

- Version bumped to 0.0.2.
- `src/` reorganised: thirteen loose `cli_*` files moved into `mcode/cli/`, the smoke test into `mcode/smoke/`, leaving only `CMakeLists.txt`, `main.cxx` and `mcode/`.
- Oversized files split by role: `loop.cxx` (837), `setup.cxx` (1043), `slash.cxx` (642), `search_tools.cxx` (627) and `exec.cxx` (560).
- `event_log` retains a bounded 16 MiB of payloads in memory, oldest first, instead of growing for the life of the process.
- `read_file` allocates to the file's size instead of 8 MiB plus one on every read.
- Approval-precedence resolution and the snapshot directory constant each have one definition, in `exec_internal`.
- CI actions updated to current majors: `checkout` v7, `setup-python` v7, `upload-artifact` v7, `download-artifact` v8, `cache` v6.
- `THIRD-PARTY-NOTICES.md` names every dependency's licence as declared by its Conan recipe, rather than deferring to the packages.
- Release archives carry `extensions/` next to the binary, which is where the binary resolves them from; the `package` job smoke-tests the unpacked archive so a missing directory fails the build rather than the user.

### Fixed

- Every `bash` call took ~20.5 s to start, because the child was granted the user's entire `%TEMP%` and an integrity label propagates to every entry beneath the path; measured 20,770 ms of a 21,015 ms spawn, now 1 ms.
- `--continue` in the interactive path never restored the conversation, so the model started from an empty history while the report claimed it had carried the messages.
- `event_log::open` merged two sessions when repointed at a second non-empty file, concatenating their transcripts.
- `mcode.fs.read` and `mcode.fs.write` could never succeed, building `{"path":/tmp/x}` without quotes; the one site of 77 missing them.
- The SSE parser enforced no bound on event size and cleared an over-long line, leaving itself mid-stream.
- The TUI dropped typed characters preceding a paste-marker prefix split across a read boundary.
- `/export` could write outside the workspace via an absolute path or `..`.
- `grep` scoped to a directory scanned zero files because `\\?\` was prefixed to a path containing a forward slash, returning a well-formed empty result.
- `parse_command_line` refused a quoted metacharacter as "compound", rejecting valid commands including every `python3 -c '...; ...'` one-liner.
- `parse_command_line` mishandled backslash escapes inside double quotes, so `\"` ended the string early and a following `||` read as an unquoted pipe.
- `finish_run` emitted invalid JSON for every run summary, a stray quote after a numeric field and no closing quote for the reason.
- `event_log::append` did not validate its payload, which is why the malformed summaries went unnoticed.
- A completed run reported zero steps, because `turn_outcome::steps` was assigned only on the done and handoff branch.
- A pipe that hit EOF was reported a full timeout late; the read-completion error branch left the timer pending.
- An extension tool handle was a position, so any unregister renumbered every later handle and a saved handle named a different tool.
- A failed extension API registration left dangling pointers in the hook and provider registries, unlike its two sibling failure paths.
- A JSON Pointer for schema validation was built from the model's own key, so an argument named `a/b` validated against `properties.a`.
- `strictify_schema` default-constructed `properties`, making a no-argument tool return `{"type":"object","properties":null}`.
- Compaction reported a drop it had not made, and could do so every step, on a usable window that had gone negative.
- `Retry-After` overflowed to a negative value past ~9.2e15 seconds and silently fell through to the short backoff.
- `glob`'s `max_results` overflowed its own walk budget, so asking for more results returned fewer.
- `build_interactive_loop` left the process wedged after any failure, reporting "an interactive session is already running" on a retry.
- `edit` spliced line endings by index, merging lines when a replacement changed the line count while still reporting success.
- `sanitize_utf8` advanced by the claimed length of a rejected sequence, truncating the text after it.
- A repetition steer dropped the looping message but left its tool calls pending, so the harness ran tools it had just deleted from history.
- The cross-turn repetition signal compared a normalized string against a raw one, making it dead at 0.885 against a 0.90 threshold.
- `exit_code_for_run` checked loop state before the budget flag, so a resumable budget stop exited 4 instead of the documented 3.
- An MCP server that died while idle was never detected, because `pump( 0 )` issued no read and `alive()` had no callers.
- MCP `tools/list` was unconditional, failing a legal resources-only server, and its pagination was unbounded.
- The SSE chunked decoder's `pending_` grew without bound while a chunk-size line never terminated.
- A 2xx that was not exactly 200 was rejected as a protocol error while still filling `failure`, which the header says cannot happen.
- `default_secret_deny` compared `.env`, `id_rsa` and `.aws` without folding case, so `.ENV` bypassed the deny on a case-insensitive filesystem.
- `std::regex` is not time-bounded, so a nested-quantifier grep pattern is refused before it runs, measured at 13 s on a 26-character line.
- `read`'s window is bounded by lines, so one minified line put its whole 16 MiB into the context; a line is now cut at a code point and the result says so.
- `splice_section` recognised the `[model]` header by exact line equality, so `[model] # my provider` gained a second section the loader rejects.
- `restore_transcript` attached every tool result to the first call and matched a log kind no writer produced, losing failed calls on resume.
- The Lua watchdog's `budget_scope` was not depth-counted, so a nested dispatch disarmed the outer deadline.
- `cli_session` passed `skills_context` by address out of a frame that died before the loop ran.
- `flush` committed before reconciling the region height, leaving a blank gap that grew with the region's old height.
- The opening screen painted the banner and then scrolled it off, and the region erased and scrolled four rows it had never covered.
- `decode_key_bytes` and the optional-argument guards: `lua_type` reports `LUA_TNONE` for an absent argument, so an omitted argument was treated as a wrong-typed one and raised.
- `mcode setup` silently ignored an unknown flag on the interactive path, so a typo'd `--no-verfy` silently verified.
- `mcode setup`'s scripted path called `std::exit( 2 )` from inside a lambda, skipping every destructor in the frame.
- An unknown command-line argument printed a bare usage dump with no explanation on the TUI path.
- `landlock_add_rule`'s errno was named `open_error`, sending a reader to debug the wrong call.
- `tui-screen` inherited `%APPDATA%` and failed on any unconfigured machine, reporting a rendering defect where the cause was a missing config file.
- `tui-screen` failed while reporting its verdict, because a CI runner's stdout defaults to cp1252 and the meter carries U+25B8.
- Five tests failed on Windows only, because Clara parses a leading `/` as an option.
- The first-run message named neither the config file's path nor the `mcode setup` command that writes one.
- The release archive omitted `LICENSE`, which Apache-2.0 section 4(a) requires a redistributor to include.
- A dependabot `commit-message.include` value that is not a legal value for that key.
- The released archive shipped no `extensions/`, so the binary registered no model provider and could not run a single turn; the archive now carries them and both installers place them beside the binary.
- The startup smoke test crashed with an access violation when the fixture or bundled extension directory was absent, killing the process part-way through and truncating every check after it; it now resolves the bundled directory from the running executable and skips only what it genuinely cannot reach.
- `_clgate.py` failed the whole run on an MSVC switch it did not list (`/WX`), reporting `no such file or directory` instead of a diagnostic about the code; the warning-switch family is now matched rather than enumerated.

### Removed

- `enum class resolution` and its `to_string`, which had no caller.
- `tool_registry::clear` and its test, which had no production caller and only proved `std::vector::clear`.
- `compaction_result`, three fields never written and one write-only, where only `kept` was read.
- `assemble_request_options::recitation` and `tool_def::deferrable`, both unreachable.
- `LIVE_REGION_ROWS`, a typical height used where the floor or the actual height was wanted.
- The dead helpers left behind by the metacharacter fix in `parse_command_line`.

### Security

- Repository-local extensions are **loaded without a trust prompt**: `default_roots` includes `<workspace>/.mcode/extensions` and runs each `init.luau` at session start. `docs/12` specifies a hash-pinned grant gate for this case and it is not implemented. Use `--no-extensions` in a repository you have not read.
- Documented the Windows integrity-label propagation that made every write grant cost the size of what was granted.
- Documented that backslash escapes inside double quotes are not optional to model, and that the fix is not "consume every backslash".

### Documentation

- `docs/41-session-analysis.md`, dissecting two runs of one long task, including the model's measured behaviour and cache measurements that correct a claim in `docs/15`.
- `docs/42-agents-md-research.md`, eight primary papers plus vendor guidance behind the `/init` prompt, with prevalence numbers.
- `docs/40-non-obvious-constraints.md` gains 118-130, from the live-region painted height to the integrity-label cost.
- `docs/24` states the Linux artifact's real linking rather than the intended one; the tarball is glibc-dynamic, not static musl.
- `README.md` corrected: 43 docs, not 41, and the licence badge points at `LICENSE` rather than an in-page anchor.
- `CONTRIBUTING.md` records the two things CI catches that no local build will: the POSIX branches a Windows workstation never compiles, and the warnings-as-errors set.

[Unreleased]: https://github.com/immutex/mcode/compare/v0.0.3...HEAD
[0.0.3]: https://github.com/immutex/mcode/compare/v0.0.2...v0.0.3
[0.0.2]: https://github.com/immutex/mcode/compare/v0.0.1...v0.0.2
