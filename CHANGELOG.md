# Changelog

Notable changes to mcode. This file follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Entries are one line per change, under the type of change: **Added**, **Changed**,
**Fixed**, **Removed**, **Security**, **Documentation**.

## [Unreleased]

### Fixed

- **The slash palette lost its first entries once the command list outgrew the live region.** The region is painted from the bottom up, so the rows a clamp dropped were the top of the list, and the symptom read as missing commands rather than as an overflow. The palette now contributes a fixed `PALETTE_MAX_ROWS` (10) to the region and paints a window over the matches: pinned to the top until the selection reaches its last visible row, then following it, with `▲`/`▼` in the marker column when rows are hidden.
- **A subagent test asserted on scripted text, so it passed against a child with no working tool.** `tests/test_subagent.cxx` added its `read` definition to a local registry the loop never sees, while the child's registry is built from the loop's own; the call failed at dispatch and the assertion passed on the text of the second queued response. The definition now goes into the loop's registry and the assertion counts handler invocations.
- A second test pins the veto path a probe was written to check: a child's tool call publishes `tool_pre_call` to the parent's bus and a veto there is enforced, which the code already did and nothing covered.
- **`/init` refused to run when `AGENTS.md` already existed.** It now submits a second prompt that asks the agent to read the existing file and audit it — keep what is still true, fix what has gone stale, add only what it can point at evidence for — instead of telling the user to delete the file first.

### Changed

- `README.md` rewritten: the tool set, the extension surface and the command list are now stated as tables, the `0.0.3`/`0.0.4`/`0.1.0` release stack collapses into one current-state section, and the "not built yet" list is refreshed.

### Documentation

- **`docs/45-viability-gaps.md`**: what other harnesses prove that mcode lacks, ranked by evidence grade, with a URL per claim. The input to the next batch.
- `docs/18-lua-api.md` named a `tool.post_call` hook that does not exist; the event is `tool.result`. A documented hook name that cannot fire is the failure mode the reserved-prefix rule exists to prevent.

## [0.1.0] - 2026-10-11

The batch that wires what 0.0.x built. Eleven interfaces existed with either no
producer or no caller, each with a passing test; the dominant defect was not
missing code but unconnected code. This release connects them, ships the seven
first-party extensions `23` mandates, and adds MCP resources and prompts.

### Added

- **The `task` tool**, a bundled extension over the core spawn primitives. The child's registry is filtered to the read class by the core, so no argument can widen its capability, and its budget is carved from the parent's remainder and folded back on completion — a fan-out cannot outspend the run.
- **`mcode.bash( command, opts )`**, the one way an extension runs a process. It routes to the same core `bash` handler, so the sandbox profile, the permission gate and the output cap are shared rather than reimplemented, and it is gated on the `spawn` manifest permission. `git` and `github` use it.
- **`mcode.fetch( url, opts )`** for a URL the *model* chose. An extension's own `net.get` is bounded by its static `net:<host>` declarations, which cannot cover a host the model invents; this is bounded by the permission engine's per-domain decision plus the harness's refusal of private and link-local addresses.
- **`mcode.plan.set( text )`**, and the recitation that reads it: the goal, the plan, the modified files and the last failure are rebuilt from session state every ten steps and after a compaction, and injected as a system message ahead of the history so a compaction cannot drop the goal.
- **Model tiering.** `model.tier.plan|act|side` route by call site, each falling back to `model.model`. The `side` tier owns compaction: the dropped middle is summarized rather than silently discarded, and a failed summary call falls back to the drop.
- **MCP resources and prompts.** `resources/list`, `resources/read`, `prompts/list` and `prompts/get` on the existing client, all bounded by the same page cap and non-advancing-cursor refusal as `tools/list`. A server that declares resources gets one synthetic `<server>__resources` tool; `/prompt` lists and fills a server's prompts.
- **Four first-party extensions**: `fs-extra` (`todo`), `web` (`web_search`, `fetch`), `git` (`git_status`, `git_diff`, `git_commit`) and `github` (`gh_pr`, `gh_issue`, `gh_review`). With `task`, `skills` and `memory`, the seven-extension shipping set is complete.
- `ext/api_entries.cxx` — the whole Lua surface in one table, so a name an extension can call and a name the type stub declares are checked against each other.

### Changed

- `register_context` carries every collaborator a surface needs. Before this, `default_register_api` built an `install_request` with nine omitted, so `mcode.cmd.register` and `mcode.defer` **raised** in a real session while `net.get`, `session.snapshot`, `context.add_instructions` and `skill.register` returned `nil`.
- Extension commands reach the palette: `invoke_command` has a caller, so a command a loaded extension registered is reachable and `/help` names it.
- One `extension_collaborators` struct is installed on both the interactive and the headless path. The two used to build their own sets and drift.
- `pinned_state` is regenerated from session state rather than summarized, so it is byte-stable between rewrites and does not invalidate the prompt cache.
- `event_log`, `session_budget`'s lifecycle, the Windows pipe plumbing and the Lua entry table each move to their own translation unit; every file is again under the 600-line cap.

### Fixed

- **A stub that satisfies a test is not a wired interface.** The test helper installed a partial surface and passed against it, which is precisely how the wiring defects shipped. It now routes through `default_register_api` with the collaborators production wires, and the regression test fails against the old wiring — verified by reverting.
- `run_subagent` read the child's spend from a local budget copy the loop never charged, so a subagent was free to the run.
- `mcode.fetch` and `resources/read` return `contents`, `tools/call` returns `content`, and `prompts/get` returns `messages[].content`. One flattening handles all three; before it, only the first was read.

### Security

- `mcode.bash` is not a second exec path. It calls the core handler, so the sandbox and the gate cannot be bypassed from Lua, and an extension that does not declare `spawn` cannot run a command at all.
- `mcode.fetch` fails closed with no permission engine, refuses a private or link-local literal address before the decision, and caps the body that enters the context separately from the body that was downloaded.

## [0.0.4] - 2026-10-11

A quality pass over the whole tree, not a feature batch. The comment rule was
aspirational and had drifted: a 0.109 comment/code ratio, 59 comments narrating
the code's history, 244 blocks of four lines or more. It is now a CI gate. Every
file over 600 lines is split, thirty dead symbols are gone, and two defects
surfaced on the way. Nothing else changed behaviour.

### Changed

- Every source and test file is under 600 lines. The six test files that were over are split along subject boundaries into 21 files carrying the identical test set — same names, same tags, same assertions.
- `cli/repl_tui.cxx` extracts the render and turn-running helpers into `repl_turn.{hxx,cxx}` over a `repl_runtime` struct, so the interactive loop is a key dispatch rather than a closure over twenty captured locals.
- `platform/sandbox_windows.cxx` puts its constants and helpers in one anonymous namespace; `platform/sandbox_linux.cxx` names the ABI levels (`LANDLOCK_ABI_*`) instead of comparing against bare `2`, `3`, `4`, `5`.
- `unique_job_windows` is renamed `unique_handle_windows`. It types token handles as well as job handles, and the old name asserted the wrong thing at eleven call sites.
- `cli/commands.cxx` routes six identical failure paths through one `report_failure`, and `commands.cxx` is back under the cap as a result.
- `tools/comment_lint.py` runs in CI and fails the build on a finding; `tools/dead_code_scan.py` reports header-declared functions nothing references.

### Fixed

- **The egress proxy sent an invalid SOCKS5 reply.** RFC 1928 fixes the reply as `VER REP RSV ATYP BND.ADDR BND.PORT` and `ATYP` must name an address type; the proxy built a ten-byte zero-filled array and set only `VER` and `REP`, leaving `ATYP = 0` on every path, success and refusal alike. A strict client rejects the reply. All three paths now share one initialised array.
- **Five file-scope symbols in `mcode::platform` had external linkage with no header declaring them.** `last_error_message`, two `inline constexpr` limits and two SDDL literals were reachable from every translation unit; a second definition anywhere in the tree would have been an ODR violation. They are now internal.
- **`mcode ext deny` corrupted the trust store.** The deny record carries no root, but the parser required one, so the line the command had just written failed to load. `trust_store_for` then degraded a failed load to an empty store, which lost every grant in the file and reported the denied extension as `ungranted` rather than `denied`. Found by running the command, not by a test: the existing deny test set a root the CLI never writes. The root is now required only of an allow record, and a regression test writes exactly what the command writes.

### Removed

- Thirty symbols nothing referenced, each verified with a grep across `src/`, `tests/` and `extensions/` before deletion: `text::is_ascii_space`; `platform::apply_sandbox`, `terminate_process`, `terminate_process_tree`, `TERMINATED_EXIT_CODE`; `api_surface::providers`, `installed_servers`, `empty_providers_`; `timer_registry::active_count`; `lua_host::memory_refusals`, `time_expired`; `watchdog_state::expired`; `allocator_state::refusals`; `manifest::known_permissions`; `trust_store::revoke`; `truncate::needs_truncation`; `session_reads::size`, `clear`; `logging::parse_log_level`; `perm::canonical_path_key`; `permission::DENIED_FLOOR_MESSAGE`, `untrusted_ingested`; `config::loaded_scopes`; `proc::chunk`; `tui::split_lines`; `tty_session::raw_active`, `mouse_reporting`; `cli::session::loop`; and the `LANDLOCK_ABI_MAX`/`LANDLOCK_NETWORK_ABI` constants that duplicated a literal comparison.
- About 2,150 comment lines a reader does not need. The ratio is 0.048 in `src/` and 0.031 in `tests/`, from 0.109 and 0.058. Rationale paragraphs moved to the doc that owns the decision; a comment restating a name, narrating the next line, or describing what the code used to do was deleted rather than reworded.

### Documentation

- `AGENTS.md` §Comments states the enforceable rule — default none, history narration never, four lines maximum, no `docs/NN` citation — and points at the gate.
- `docs/40-non-obvious-constraints.md` gains 132–134: why a history-narrating comment survives review, the SOCKS5 reply, and file-scope symbols with no declaration.
- `docs/36`, `docs/39`, `docs/43` and trap 74 no longer name `apply_sandbox`, which does not exist.
- `docs/00-index.md` and `README.md` record 0.0.4 as a quality pass.

## [0.0.3] - 2026-10-11

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

[Unreleased]: https://github.com/immutex/mcode/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/immutex/mcode/compare/v0.0.4...v0.1.0
[0.0.4]: https://github.com/immutex/mcode/compare/v0.0.3...v0.0.4
[0.0.3]: https://github.com/immutex/mcode/compare/v0.0.2...v0.0.3
[0.0.2]: https://github.com/immutex/mcode/compare/v0.0.1...v0.0.2
