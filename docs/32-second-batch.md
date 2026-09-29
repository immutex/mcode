# Second Batch — Permissions, Skills, and MCP

> TL;DR: Three parallel workstreams that together make mcode **usable on a real
> repository today**, rather than at some future milestone. A permission engine
> that prompts only when it must and remembers the answer; skills and
> `AGENTS.md` so the agent knows the repository it is standing in; a stdio MCP
> client so anything mcode does not implement is reachable. The first batch
> built the spine; this one makes it worth running.

## Why this batch

The first batch shipped a working agent: a real loop, eight tools, TLS, a live
provider, 259 tests. What it did not ship is a **usable** one. Measured:

| Symptom | Cause | Slice |
|---|---|---|
| `bash` is denied unless `--yolo` | `exec_policy`'s allowlist is never populated and nothing prompts | A |
| `--yolo` is the only way to run a command | There is no approval path at all | A |
| The agent does not know the repo's conventions | Nothing reads `AGENTS.md` | B |
| No way to teach it a procedure | Nothing reads `SKILL.md` | B |
| Nothing outside the eight tools is reachable | No MCP, no network tools | C |
| Skills and MCP cannot ship in an extension | The loader wires tools, providers and hooks, but there is no skill or MCP surface for it to wire | B, C |

Two of those are the same complaint: **the harness asks too much and knows too
little.** A user cannot run a command without disabling every check, and once it
runs, the agent has no idea what this repository expects. Those are the two
things that decide whether someone keeps using it.

## The requirement, stated as an acceptance criterion

From the user, verbatim in intent: *permissions should have an automatic/yolo
mode; the security systems must not be "bothering" the user; add ways to
remember permissions.*

That is three testable properties, and they are the acceptance criteria for
slice A:

1. **The common path prompts at most once per distinct command.** A typical
   session — read files, edit files, run the project's test command — must
   complete with **zero prompts once the answers are remembered**. The first
   occurrence of a distinct command may prompt; the second, in the same session
   or a later one, must not.
2. **A prompt is a question, not a wall.** It shows exactly what will run,
   offers "always", and persists that answer so the question is asked once.
3. **Unattended runs are safe by default.** Headless with no answer available
   denies and says why, rather than hanging or allowing.

## Ordering, and the decisions that were open

Three decisions were open when this batch was drafted. All three are resolved in
`docs/33`, and none of them blocks a start:

| Decision | Resolution |
|---|---|
| Does an in-workspace edit prompt? | **No.** Workspace writes are `allow`; prompts are for commands and for writes outside the workspace (`33` §Resolved) |
| What does `--yolo` actually disable? | Prompts only. A small **hard-deny floor** survives it, so `--yolo` is "stop asking me", not "no policy" (`33` §The hard-deny floor) |
| Which API additions are justified? | Both. `skill.read`/`skill.list` and `mcp.register`, on `18`'s own `model.register` precedent, with `api_version = 1` never shipped |

Everything else follows a doc that already exists.

## The three workstreams

| | Slice | Owns | New files | Depends on |
|---|---|---|---|---|
| **A** | Permissions, approvals, remember store | `perm/`, `tools/exec_policy` (folded in), the loop's check | 12 | — |
| **B** | Skills and instruction chain | `skills/`, `instruct/`, prompt sections 10–11, `extensions/skills/` | 13 | — |
| **C** | MCP stdio client | `mcp/`, `proc/session`, `[mcp]` config | 15 | Phase 0 (P1) |

**They overlap in exactly two files.** A owns the decision path, B owns the
prompt's data sections, C owns a new subsystem; no file is shared between A and
either of the others. B and C both touch `ext/api.cxx` (one `ENTRIES` row each)
and `ext/api.hxx` (one declaration each) — **four lines in total**, which
integration resolves. Everything else they need is their own file, which is why
each creates `api_skill.*` / `api_mcp.*` rather than editing one shared file.

`src/cli_commands.cxx` is shared by all three and is integration's.

**New files** counts rows in each slice's `## Files` table — headers and sources
together, so a `.hxx`/`.cxx` pair is two. A few rows are edits to existing files
rather than additions; the number is the size of the slice, not of the diff.

## Phase 0 — the one change that must land before any branch

Same discipline as the first batch: an interface invented twice is a rewrite at
merge. Here the genuinely shared surface is smaller than the first batch's, and
it is one item.

| # | Change | Why shared | Blocks |
|---|---|---|---|
| **P1** | `tool_class::mcp` added to `core/registry.hxx` and `to_string` | C registers MCP tools and needs a class to declare them. A's default rule set sends that class to `ask`, so A references it too. Without it, both branches add the same enumerator and one of them loses | A, C |

**P4, P2/P3 and P5 are deliberately *not* Phase 0.** An earlier draft put them
here and it was wrong for a reason worth recording, because the mistake is
attractive:

- **`tool_context` gains `permission_engine*` / `approval_source*` (P4)** cannot
  precede slice A — those types are A's. A owns the edit; the other two slices
  never touch it.
- **The API rows for `skill.read`/`skill.list` and `mcp.register`** cannot land
  early either. A row in `ENTRIES` names a function; landing the row without the
  body is a link error, and landing a stub that returns "not implemented"
  advertises a capability the surface does not have — the freeze exists precisely
  to stop the surface and the implementation drifting apart. So **each slice adds
  its own row and its own handler file**: B creates `ext/api_skill.{hxx,cxx}`, C
  creates `ext/api_mcp.{hxx,cxx}`, and the only shared edit is one or two lines
  in the `ENTRIES` table, which integration resolves.
- **`[mcp]` in `CONFIG_SECTIONS`** looks like harmless data, and it is not.
  Adding the section before anything reads it means a user can write `[mcp]`,
  have it accepted, and have it silently ignored — which is exactly the failure
  the section allowlist exists to prevent (`22`: *"storing it silently means the
  user believes a setting took effect when it did not"*). C adds the section and
  its reader in the same commit.

The lesson, recorded because it generalises: **a Phase 0 item must be complete in
itself.** An enumerator is; a row pointing at a missing function is not; a config
section nothing reads is not.

## File ownership, and the two files that would otherwise collide

Every file has exactly one writer. The audit that produced this table found two
collisions, both fixed rather than tolerated:

| Would collide | Fix |
|---|---|
| `cli/exec.{hxx,cxx}` — A's `--approval` / `--add-dir` flags, B's `mcode skill list\|validate` | `cli/exec.*` is **A's**. B puts its command in `cli/skill_command.{hxx,cxx}` and adds one dispatch branch to `main.cxx`, which is B's |
| `ext/api.cxx` — B's `skill.*` rows, C's `mcp.register` row | Each slice creates its own `api_skill.*` / `api_mcp.*` and adds its own `ENTRIES` row in the same commit, so no slice is a stub; the two rows are resolved at integration |

`src/cli_commands.cxx` is the third shared file and it is **integration's** by
assignment, not by accident: all three slices need to construct something in it.

## Sequencing

**Phase 0 → A → (B ∥ C) → integration.**

- **A first, alone.** It changes the dispatch path every tool goes through. B and
  C both add tools, so building them on an unchanged dispatch path and then
  re-basing on A is a merge for no benefit. A is also the slice that unblocks
  daily use, so it should not wait behind anything.
- **B and C run in parallel** once A lands. They share no file.
- **Integration last**, and it is its own commit with its own acceptance test.

**If the batch has to be cut short, the order is A, then B, then C.** A makes the
tool usable; B makes it know the repository; C adds reach that `bash` already
partly covers. C is written to be deferrable — it touches nothing A or B touch,
and deferring it leaves no half-built seam.

## Integration — what it owns

`src/cli_commands.cxx` is the only file all three need, so it belongs to the
integration commit, not to a slice. Same shape as the last batch's
`run_exec` work:

```
config (merged) ──► permission store ──► permission_engine
                            │
approval_source ◄───────────┘   (terminal | headless)
        │
        ▼
  tool_context ──► loop ──► registry ◄── mcp source (C)
                                ▲
                                └── skills extension (B) ──► skills index ──► prompt (B)
```

The integration commit must also settle three things the slices cannot:

1. **The store's location.** `%APPDATA%\mcode\permissions.json` (user) and
   `<repo>/.mcode/permissions.json` (project). A created before B or C exist, so
   it is the integration's job to keep them consistent with the config scopes.
2. **Which approval source is chosen.** Terminal when stdout is a TTY and
   `--json` is absent; headless otherwise. This is a policy decision with a
   security consequence, so it is integration's, not a slice's.
3. **Order of discovery and construction.** The extension loader must run before
   the prompt is assembled (B's index is conditional on the `skills` extension
   being loaded), and after the permission engine exists (C's MCP servers need
   consent).

## Acceptance — the batch is done when

Each slice carries its own acceptance tests in its plan. The batch-level test is
the one that proves they compose, and it is a **live run**, because the last
batch's evidence is unambiguous: every defect the suite missed was found by
pointing the harness at something real.

**The acceptance run:**

1. In a repository containing an `AGENTS.md` and a `.mcode/skills/` directory,
   run `mcode exec "…"` with a prompt requiring a skill and a shell command.
2. The agent follows a convention stated only in `AGENTS.md`.
3. The skill index appears in the request and the model calls `skill_read`.
4. The shell command **prompts once**, the answer is "always", and a second
   identical command in the same session does not prompt.
5. A second run, fresh process, does not prompt for that command at all —
   the store was written and re-read.
6. Exit 0.

Plus the three properties from the requirement, each asserted:

| Property | Test |
|---|---|
| The common path prompts at most once per command | A read/edit/test-command session records **at most one** prompt per distinct command, and **zero** on a repeat run once the store is populated |
| Remembering works | The same command twice in one session prompts once; across sessions, zero |
| Unattended is safe | `--json` with an `ask` fails that tool call naming the rule and the run continues; exit 5 only when a denial leaves the loop unable to progress. Never a silent allow, never a hang |
| Yolo is not no-policy | `--yolo` with `rm -rf ~` is still denied; `--yolo` with `rm -rf ./build` is allowed. Both halves — the second is what keeps the floor from being a nuisance |
| The floor does not misfire | Every rule in `33` §The floor's own tests is checked against its near-miss: `~/scratch/project-a` is deletable, `.gitignore` is editable |

## A doc inconsistency found while planning, recorded not fixed

`AGENTS.md` says the budgets are *"defined in `docs/01-north-star.md`; never
redefine them; reference them"*, and lists a session-start row of ≤8.5K. But
**`docs/01` contains no session-start row.** Its table covers cold start, RSS,
binary size, dependency count, tool schema, TTFT and extension load cost.

The 8.5K figure lives at `docs/05:71`, which marks that row *"the authority"*.
So one of the two is wrong: either `docs/01` is missing a budget it is supposed
to own, or `AGENTS.md` points at the wrong doc.

This batch cites `docs/05:71` directly, because that is where the number is
written and where the breakdown (prompt 1.5K + tools 3.5K + instructions 2K +
skill index 1.5K) lives. **The underlying inconsistency is not fixed here** — it
predates this batch, it touches `AGENTS.md`, and picking the resolution is a
one-line decision for the maintainer rather than something a planning pass
should settle silently.

## What this batch does not do

Stated so it is not mistaken for an oversight:

- **No OS sandbox.** Landlock, Seatbelt and the Windows restricted token are
  still `16` M6. The permission engine is a *gate*, and `--yolo` therefore means
  "I trust this repository", not "still sandboxed". The help text and README must
  say that plainly — claiming otherwise is a false security claim.
- **No egress proxy.** `net` tools do not exist yet, so there is nothing to
  police. When `web` ships, the proxy ships with it.
- **No extension trust grants.** Hash-pinned, view-then-grant for
  `.mcode/extensions/` is `12` §T3 and needs a trust DB this batch does not
  build. MCP servers do get an explicit-consent path, because their command line
  is a code-execution vector.
- **No MCP over HTTP.** Stdio only; `07`'s reasoning is that a coding CLI's
  servers are local.
- **No session resume, no snapshots, no evals growth.** Those are M3/M4 and are
  untouched here.

## Risks

| Risk | Impact | Mitigation |
|---|---|---|
| The workspace-write default is wrong | Either prompts on every edit (unusable) or allows too much | It is one config line either way; the decision is called out for explicit sign-off, and both behaviours are tested |
| Three API additions in one batch looks like drift | The freeze loses its meaning | Each is justified against `18`'s own `model.register` precedent and the fact that `v1` has not shipped; the doc records the new count |
| Slice A changes the dispatch path B and C build on | Rebase churn | A lands first and alone; B and C branch after it |
| MCP's schema cost eats the tool budget | Context bloat, degraded tool choice | New servers default off, per-server token estimate warned above 8K, lazy loading explicitly deferred with its reason |
| Skills index and instruction chain exceed the 8.5K session-start budget | Cold-start budget breach | Both are measured; a test asserts the sum with a realistic chain plus 15 skills |
| The permission engine's defaults annoy rather than protect | Users reach for `--yolo` permanently, which is worse than no gate | That is the requirement, and it is the first acceptance criterion |

## Shipped — what landed, and what the plan got wrong

All three slices landed together and are green (344 tests, smoke, `_clgate.py`).
Four things the plan did not anticipate, recorded because each is a defect class
rather than a one-off:

1. **`docs/33`'s file table left `agent/loop.{hxx,cxx}` unassigned.** Both A and
   B need them — A for the engine pointer on the loop, B for the prompt's data
   sections — and both added `instruction_chain_`/`skill_index_` independently,
   which was a redefinition at merge. One writer per file would have avoided it;
   the interface was settled mid-flight instead.

2. **`src/main.cxx` was shared by A and B in disjoint regions**, which the table
   did not say. `cli_commands.cxx` was correctly reserved for integration, but
   `main.cxx` holds both the `exec_policy` construction and the subcommand
   dispatch.

3. **An extension tool needs a handler routed back into its VM.** `load_result`
   had `invoke` and `tool_owners` all along; nothing wired them to the loop, so
   an extension tool appeared in the schemas and every call returned "tool has no
   handler registered". Every test passed. It was found by pointing the harness
   at a real repository — the same way the first batch found its defects — and
   `skills` is the first bundled extension to register a *tool* rather than a
   provider, which is why it had never surfaced.

4. **`mcode skill` was written, tested and unreachable.** The dispatch branch was
   lost to a worktree revert and never restored; 344 tests were green because
   they call `run_skill` directly and never go through argument dispatch.

The batch's own acceptance criteria, as verified:

| Criterion | Evidence |
|---|---|
| Workspace edits never prompt | `test_permissions` default-set assertions; the deviation is in `README` §48 |
| A command prompts once, then never again | `an mcp call remembers per tool…` and the store round-trip tests; the store keys are written and read through one `store_key_for` |
| A remembered allow is argv-exact | `a remembered allow is argv-exact` |
| `--yolo` still denies the floor | `yolo allows an ask but still denies the floor`, plus the near-miss table in both directions |
| Headless fails closed and names the rule | `headless fails closed and the model is told why` |
| A project store cannot widen | `a project store cannot widen: allows are dropped with a warning` |
| The instruction chain reaches the model, closest-wins | live turn: the agent followed a convention stated only in the fixture's `AGENTS.md` |
| The skill index is emitted only when `skill_read` is registered | `the index is emitted only when skill_read is registered` (both directions) |
| The session-start budget holds | 15 skills plus a realistic chain, asserted inside 8.5K |
| An MCP server's tools register and are callable | `test_mcp` through the fixture binary, including crash-restart, timeout, banner and hash-pin |

**Not achieved, and why.** The batch-level acceptance run's step 4 — answer
"always" at a real prompt, then confirm a second identical command does not
prompt — was not driven end-to-end, because the cheap model available here will
not reliably emit a tool call; it answered from memory twice instead. The
mechanism is covered by tests in both directions (same session, and a fresh
engine over the same file), but the interactive prompt has not been exercised by
hand.

## Sources

- `docs/12-security.md` — the permission engine, the decision table, scope precedence, the sandbox deferral
- `docs/22-config-and-cli.md` — config scopes, the never-widen rule, the CLI surface and exit codes
- `docs/08-skills-and-agents-md.md` — skills and instruction-chain design
- `docs/07-mcp.md` — the MCP client design, transports, token measurements
- `docs/18-lua-api.md` — the frozen surface and the `model.register` amendment precedent
- `docs/23-first-party-extensions.md` — the core/extension split, and the per-extension failure/disablement rules
- `docs/16-roadmap.md:110` and `docs/19-extensions.md` — the disable-all test, which those two define
- `docs/21-system-prompts.md` — prompt assembly order and conditional sections
- `docs/16-roadmap.md` — M1 permission UX, M2 context discipline, M5 extension runtime, M6 sandbox enforcement, M7 skills and MCP
- `docs/26-first-batch.md` — the first batch, its Phase 0 discipline, and the integration acceptance pattern
- `docs/05-context-engineering.md:71` — the 8.5K session-start row this batch must not exceed
