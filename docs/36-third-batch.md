# Third Batch — Interactive surface, Lua API, and the OS sandbox

> **Status: shipped.** All three workstreams landed and the batch acceptance
> run passed. The record of what actually shipped, including the three places
> the plan was wrong, is in §Outcome below.

> TL;DR: Three workstreams that turn a working batch tool into a daily driver.
> An **interactive TUI** so you can hold a conversation instead of firing one
> shot per process; the **Lua API completed** so the "C++ primitives, Lua
> ecosystem" thesis is actually provable; and the **OS sandbox** so `--yolo`
> stops meaning "I trust this repository" and starts meaning "the kernel is
> holding the line". The first batch built the spine; the second made it usable
> on a repository; this one makes it something you would run all day.

## Why this batch

Measured against the tree, not the docs. Three gaps, each of which blocks a
different claim.

| Symptom | Cause | Slice |
|---|---|---|
| You cannot ask a follow-up question | There is no interactive mode. `mcode exec` runs one task and exits; `ask_user` returns an error headless | A |
| The agent cannot show you anything mid-run | No renderer. Deltas are mirrored raw to stdout by a 40-line adapter | A |
| An extension author writes against `mcode.fs.read` and it fails at runtime | 13 of 23 declared entry points are unimplemented | B |
| `--yolo` is a policy check on parsed argv, not isolation | `apply_sandbox` returns `unsupported` on **all three platforms** | C |
| Nothing can be extended without recompiling | `cmd`, `timer`, `cfg`, `fs`, `net`, `session` are declared and absent | B |

The three are independent and each is the top of its own list. A is the single
biggest usability wall; B is the load-bearing claim of the whole design; C is
the largest unhedged risk.

### What "close to finishing" actually means

Stated plainly so the batch is not mistaken for the end. Of `16`'s nine
milestones, M0–M2 and M7 (skills/MCP half) are done, M3/M4/M5 are partial, M6
and M8 are untouched. This batch completes M5 and the sandbox half of M6, and
delivers M7's remaining headline item. After it the harness is usable, safe and
extensible — roughly 80% of the design. What remains is reach and reliability:
session resume, the eval suite in CI, extra providers, subagents, memory,
distribution, and the extension registry.

## The requirement, stated as acceptance criteria

From the user's goal, in intent: *a lightweight C++23 coding-agent harness that
is reliable and scalable, in the class of the tools people already use daily.*

Three testable properties, one per slice:

1. **A conversation is possible.** `mcode` with no arguments starts an
   interactive session: type a task, watch it stream, answer an approval
   prompt, then type a follow-up **in the same process, with the history
   intact**. Ctrl+C interrupts the turn, not the session.
2. **The declared surface is the real surface.** Every entry point in
   `extensions/mcode.d.luau` either works or is not declared. An extension can
   register a command, schedule a timer, read config, read and write a file,
   and snapshot the session — without recompiling the harness.
3. **The sandbox is not a claim.** `sandbox_capability_level()` reports what this
   machine actually enforces, `apply_sandbox` restricts a spawned child on all
   three platforms, and the help text, the README and the seam all agree.

## The three workstreams

| | Slice | Owns | New files | Depends on |
|---|---|---|---|---|
| **A** | Interactive TUI | `tui/`, `cli/repl`, `cli_commands.cxx`, `main.cxx` | ~14 | — |
| **B** | Lua API completion | `ext/api_*`, `ext/api.{hxx,cxx}`, `ext/loader.{hxx,cxx}`, `mcode.d.luau` | ~12 | — |
| **C** | OS sandbox + egress | `platform/sandbox_*`, `platform/seams`, `proc/`, `tools/exec_tools.cxx` | ~6 | — |

**They overlap in exactly one file: `src/CMakeLists.txt`.** That is the whole
shared surface — three contiguous blocks of source rows. Everything else is
disjoint, which is why this batch has no Phase 0 (below).

## No Phase 0, and why that is the finding

The second batch needed one (`tool_class::mcp`) because two slices both wanted
an enumerator in a shared header. This batch needs **none**: A touches the
terminal and the CLI entry, B touches the extension surface, C touches the
platform seams, and no interface is invented twice.

The rule from last batch still applies — *a Phase 0 item must be complete in
itself* — and the corollary is that an empty Phase 0 is a real result, not an
oversight. If a slice finds it needs something shared, that is a signal to stop
and report, not to grow Phase 0 after the fact.

## File ownership

Every file has exactly one writer. Last batch lost time to an unassigned
`agent/loop.{hxx,cxx}` that two slices both edited, so this table is exhaustive
rather than indicative.

| Path | Owner | Note |
|---|---|---|
| `src/mcode/tui/**` | A | the renderer, cell buffer, tty layer, input parser, markdown, diff view |
| `src/mcode/cli/repl.{hxx,cxx}` | A | the interactive session loop |
| `src/mcode/tui/approval_tui.{hxx,cxx}` | A | implements `perm::approval_source`; **reads** `perm/approval.hxx`, never edits it |
| `src/cli_commands.cxx` | **A** | A is the only slice that needs it — the REPL reuses the same construction. B and C are forbidden from touching it |
| `src/main.cxx` | A | the no-subcommand branch |
| `tests/test_tui_*.cxx` | A | |
| `src/mcode/ext/api_{fs,net,cfg,cmd,timer,session}.{hxx,cxx}` | B | one file pair per namespace |
| `src/mcode/ext/api.{hxx,cxx}` | B | `ENTRIES` rows and `install_request` fields |
| `src/mcode/ext/loader.{hxx,cxx}` | B | the new collaborators reach `install_request` here |
| `extensions/mcode.d.luau` | B | the declaration file must match the surface exactly |
| `tests/test_api_*.cxx` | B | |
| `src/mcode/platform/sandbox_{windows,linux,macos}.cxx` | C | |
| `src/mcode/platform/seams.{hxx,cxx}` | C | `apply_sandbox`, `sandbox_capability_level`, `sandbox_network_level`, `sandbox_mechanism` |
| `src/mcode/proc/**` | C | spawn with a profile applied |
| `src/mcode/tools/exec_tools.cxx` | C | the sandboxed exec path |
| `tests/test_sandbox.cxx` | C | |
| `src/CMakeLists.txt`, `tests/CMakeLists.txt` | **shared** | append-only, contiguous blocks, no reformat |
| `docs/00-index.md`, `README.md` | **integration** | |

## Sequencing

**A ∥ B ∥ C.** No slice depends on another, and A's risk is design rather than
integration, so there is nothing to serialise.

Within a slice, the ordering that matters:

- **A: tty layer → cell buffer → frame diff → commit protocol → markdown →
  input editor → approval source → REPL wiring.** The tty layer is the only
  part that cannot be tested without a terminal, so it is built first and
  behind an interface that a fake can satisfy.
- **B: permissions plumbing → `fs` → `cfg` → `cmd` → `timer` → `session` →
  `net` → `context.add_instructions` → `/reload`.** Cheapest and most testable
  first; `net` last because it needs the HTTP client and a domain policy.
- **C: Windows restricted token → Linux Landlock → macOS Seatbelt → egress
  deny.** Ordered by how much can actually be verified on the machine this is
  built on, which is Windows.

## Integration — what it owns

Small this batch, because A owns `cli_commands.cxx`:

1. Reconcile the three `CMakeLists.txt` blocks.
2. `docs/00-index.md` — mark the batch shipped.
3. `README.md` §Non-obvious constraints — the deviations and traps each slice
   produces.
4. The batch acceptance run, below.

## Acceptance — the batch is done when

Each slice carries its own tests. The batch-level test is the one that proves
they compose, and it is a **live run**, because both previous batches found
their worst defects by pointing the harness at something real:

1. In a repository with an `AGENTS.md` and a `.mcode/skills/` directory, start
   `mcode` interactively in a real terminal.
2. Type a task that requires reading a file and running a command.
3. The transcript streams; the tool call renders as a live row and commits as
   one line; the approval prompt appears **inside the UI**, not as a raw
   `stdin` read.
4. Answer "always"; the command runs; its output renders.
5. Type a follow-up. **The history is intact and no second prompt appears for
   the same command** — this is the intersection of A and the second batch's
   remember store, and it is the assertion that proves the REPL is not just a
   loop around `exec`.
6. With a sandbox-capable platform, the spawned command reports the sandbox as
   enforced, and a write outside the profile **fails**.
7. Ctrl+C during a turn interrupts the turn and leaves the session usable.
8. Exit 0.

Plus the three properties from the requirement, each asserted:

| Property | Test |
|---|---|
| A conversation is possible | The REPL runs two turns in one process, history intact, and a follow-up sees the first turn's tool results |
| The declared surface is real | A test walks `mcode.d.luau`'s declarations and fails if any is missing from `ENTRIES` — the check that would have caught the 13-entry gap |
| The sandbox is not a claim | `sandbox_capability_level()` matches what a probe actually observes: a denied write is denied, an allowed write succeeds |

That second row is the one to build first. It is the only mechanical defence
against the failure mode this batch exists to fix, and it is cheap.

## Outcome

Shipped. The evidence, all run on Windows/MSVC:

| Gate | Result |
|---|---|
| `cmake --build build/Release -- -j 1` | 0 errors |
| `mcode_tests.exe` | 2322 assertions, 396 test cases, 0 failures |
| `mcode.exe --smoke` | 141 checks, 0 failures |
| `mcode.exe eval` | 10 passed, 0 failed |
| `_clgate.py` | exit 0 |
| binary / idle RSS | 8.25 MB / 13.4 MB (budgets: ≤25 MB, ≤30 MB) |
| **CI: Linux GCC 14, macOS arm64, Windows MSVC** | **all green** |

The POSIX legs are the ones worth noting. They were red for fifteen CI cycles
after the Windows build was green, because `_clgate.py` compiles with `_WIN32`
defined and this machine has no POSIX toolchain — every defect below the
platform seam surfaced one run at a time. The last four were real product bugs
that had never executed anywhere: `sandbox_init` receiving a profile as a
filename, Seatbelt not resolving the `/private` symlinks, Landlock refusing a
rule for a character device and failing the whole ruleset, and POSIX `exec` not
searching `PATH`.

### What each workstream delivered

**A — the interactive surface.** `src/mcode/tui/` (tty, cell, theme, frame,
markdown, diff_view, editor, approval_tui, render) plus `cli/repl`, wired into
`run_repl`. `diff_view` and the markdown block parser were later removed as
unwired scaffolding — see `37` §Markdown and diffs. The loop runs on a worker
thread per turn and the renderer pumps on the main thread; the bus stays
single-threaded because its handlers run on the loop thread and push copies
into a mutex-guarded queue. Two turns in one process carry history — asserted,
and confirmed live against the endpoint. `mcode exec` is byte-unchanged.

**B — the Lua API.** The declared surface is now real in both directions: the
declared-vs-implemented test **parses `extensions/mcode.d.luau` at test time**
rather than comparing against a hand-copied array, and it passes empty.

**C — the OS sandbox.** Windows confines writes via a Low-integrity token
applied through `CreateProcessAsUserW` plus mandatory-label marking, and
reports `write_boundary` — writes confined, reads not, because integrity levels
have no read-down restriction. Linux (Landlock with mandatory ABI detection)
and macOS (deny-default Seatbelt) are **verified by CI on every push**: both
legs run the full suite, so a POSIX regression fails the build rather than
waiting for a release.

### Three places the plan was wrong

1. **Windows cannot report `filesystem`.** The plan assumed a restricted token
   with a synthetic SID. That SID is granted by no DLL, so the second access
   check kills the child at load. The ladder gained a `write_boundary` rung,
   and `PROC_THREAD_ATTRIBUTE_TOKEN` — which the earlier code used — **does not
   exist in the SDK** (5 is `IdealProcessor`), so the token had never been
   applied at all. Found by a probe, not a review.
2. **The missing-entry count was 12, not 13.** Diffing the declaration file
   against `ENTRIES` showed `defer` and `notify` were missing from the
   worklist entirely, and `cmd.handler` — which the hand-copied test array
   listed — is a *parameter name* of `on`, not an entry point. The test's
   mirror was wrong in both directions, which is the argument for parsing the
   declaration rather than copying it.
3. **`mcode` with any option was unreachable.** The dispatch rejected every
   argument that was not a known subcommand, so `--yolo` and every other
   session flag failed. The plain fallback also discarded every answer and
   denied every tool call. None of the three had a test; all three were found
   by the live acceptance run, which is why the batch criteria insist on one.

## What this batch does not do

Stated so it is not mistaken for an oversight:

- **No session resume.** The REPL keeps history in memory; `mcode` still cannot
  restore a session from JSONL. That is M3 and it is the next batch's first item.
- **No eval suite in CI.** M4's 30–100 fixtures and the trajectory evals are
  untouched; the 10-task suite stays a local gate.
- **No extra providers, subagents, memory, or ACP.** M8.
- **No distribution or registry.** M8, and `25` calls the lockfile format
  irreversible, so it needs its own milestone rather than a corner of this one.
- **No `git`/`github`/`web` bundled extensions.** They become possible *because*
  of B — that is the point of B — but writing them is the next batch's proof
  that the API is sufficient, not this batch's deliverable.
- **No trust grants for project extensions.** M6's other half; it needs a trust
  DB this batch does not build.
- **No MCP over HTTP.** Still deferred, still by design.

## Risks

| Risk | Impact | Mitigation |
|---|---|---|
| The TUI is ~3–5k LOC and is the largest single slice | The batch overruns on one slice | Ordered so every layer is testable behind an interface; the tty layer is the only untestable part and it is ~300 lines. If it must be cut, cut markdown highlighting and the diff word-LCS — the REPL stays useful without them |
| The loop may not support consecutive turns in one process | A's whole premise | Verified as A's first task, before any rendering work. If it does not, that is a loop change A owns and reports |
| A renderer cannot be verified without a terminal | Silent breakage | Render to a buffer, assert on the emitted ANSI byte stream, and keep a `--render-to` debug flag so the output is diffable in CI |
| Linux and macOS sandbox code cannot be run on the build machine | Shipping unverified code | C implements Windows first and fully verifies it; Linux and macOS are compile-verified by the gate and CI-run, and C reports explicitly what it could not execute |
| Landlock ABI detection is mandatory, not optional | A wrong ABI silently no-ops | Runtime detection with a hard failure when the kernel reports an ABI the code does not handle; never assume |
| Seatbelt has no supported replacement and profiles are easy to make escapable | A false security claim | `(deny default)` only; `sandbox_capability_level()` reports the truth and the seam already fails closed |
| Three slices appending to two `CMakeLists.txt` | A merge conflict for no benefit | Append-only contiguous blocks, fixed anchor points, stated in every brief |
| The declared-surface test is written last | The gap it guards reappears | It is the first thing B builds |

## Sources

- `docs/13-cli-and-tui.md` — the TUI architecture, cell buffer, commit protocol, theme tokens, glyph rules
- `docs/18-lua-api.md` — the frozen surface, the 23 declared entry points, the `model.register` amendment precedent
- `docs/19-extensions.md` — loader, lifecycle, error containment, quarantine, the disable-all test
- `docs/12-security.md` — the sandbox decision table, per-platform mechanisms, the egress argument
- `docs/24-cross-platform.md` — minimum OS versions, Landlock ABI, Seatbelt deprecation, ConPTY floor
- `docs/16-roadmap.md` — M5, M6, M7, M8 deliverables and exit criteria
- `docs/17-lua-runtime.md` — VM lifecycle, JIT policy, reload semantics
- `docs/23-first-party-extensions.md` — the core/extension split and the bundled catalog
- `docs/32-second-batch.md` — the previous batch, its Phase 0 discipline, and the post-ship audit
- `extensions/mcode.d.luau` — the declared surface, measured against `ext/api.cxx`'s `ENTRIES`
