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
| **A** | Permissions, approvals, remember store | `perm/`, `tools/exec_policy` (folded in), the loop's check | 12 | Phase 0 (P4) |
| **B** | Skills and instruction chain | `skills/`, `instruct/`, prompt sections 10–11, `extensions/skills/` | 11 | Phase 0 (P2) |
| **C** | MCP stdio client | `mcp/`, `proc/session`, `[mcp]` config | 13 | Phase 0 (P1, P3, P5) |

**They do not overlap.** No file appears in two slices. A owns the decision
path, B owns the prompt's data sections, C owns a new subsystem. They meet only
in `src/cli_commands.cxx`, which integration owns, and in Phase 0 below.

**New files** counts rows in each slice's `## Files` table — headers and sources
together, so a `.hxx`/`.cxx` pair is two. A few rows are edits to existing files
rather than additions; the number is the size of the slice, not of the diff.

## Phase 0 — the shared-interface changes, landed before any branch

Same discipline as the first batch: each slice needs something that does not
exist, and an interface invented twice is a rewrite at merge. Each is small.
**These land on `master` before `feat/*` is cut**, one commit each with its test.

| # | Change | Why shared | Blocks |
|---|---|---|---|
| **P1** | `tool_class::mcp` added to `core/registry.hxx` and `to_string` | Slice C registers MCP tools and needs a class to declare them. A reads `klass` as an opaque value and needs no new enumerator — but its default rule set must classify `mcp` as `ask`, which is a rule entry, not a code change | C |
| **P2** | `mcode.skill.read( name )` / `mcode.skill.list()` registered, with bodies in `ext/api_skill.{hxx,cxx}` | Slice B's `skill_read` is a Lua extension and must fetch a body. `mcode.fs.read` cannot do it — it is **not implemented**, and a user-scope skill is outside the workspace anyway | B |
| **P3** | `mcode.mcp.register( def )` registered, with its body in `ext/api_mcp.{hxx,cxx}` | Slice C's extension-declared servers | C |
| **P4** | `tool_context` gains `permission_engine*` and `approval_source*` | The tool handlers need to ask; integration constructs them. **Phase 0 owns the edit** — A consumes the fields | A |
| **P5** | `[mcp]` added to `CONFIG_SECTIONS` | Slice C's config section. Today an `[mcp]` section is **rejected as unknown**. **Phase 0 owns the edit** — C consumes the section | C |
| **P6** | `docs/18` records the new row count, and `docs/22` gains the `[mcp]` section | Both docs own what these changes alter, and `AGENTS.md` requires docs and code to move together. **Phase 0 owns both edits** | — |

**Where a slice's `## Files` table names a file above, it means "this slice
edits that file after Phase 0 has landed the interface"** — except P4, P5 and P6,
which Phase 0 completes outright so no branch reopens them. The distinction is
the whole point of the phase: a frozen interface is one nobody has to touch
again.

**P2 and P3 split the file so no two branches touch it.** `ext/api.cxx` holds one
`ENTRIES` table and one registration function; three writers in it is a
guaranteed conflict. Phase 0 therefore creates `ext/api_skill.{hxx,cxx}` and
`ext/api_mcp.{hxx,cxx}` with **stub bodies that return "not implemented"**, adds
the three rows to the table pointing at them, and stops. `api.cxx` is then owned
by Phase 0 alone: B fills in `api_skill.cxx`, C fills in `api_mcp.cxx`, and
neither opens the table.

**P2 and P3 amend the frozen API surface.** `docs/18` freezes it at 23 rows
and treats additions as the thing to resist. Two additions at once needs the
justification stated rather than assumed:

- The precedent is in the doc itself. `mcode.model.register` was added after the
  initial freeze, and `18`'s own conclusion is *"a freeze declared before the
  feature that needs it is premature… freezing after `D2` would have produced 23
  the first time."* That is exactly this situation: the surface was closed before
  the skill and MCP seams were built, and both now need an entry.
- **`api_version = 1` has never shipped.** Nothing published depends on the row
  count, which is the condition `18` sets for an amendment being legitimate
  rather than a loosening.
- Both **complete an existing namespace**: `mcode.skill.*` and `mcode.mcp.*`
  already have rows in the table. Neither widens the root.

After this batch the surface is **29 callable names plus 3 fields** — 26 today,
plus `skill.read`, `skill.list` and `mcp.register` — and the freeze is real: any
further addition needs a shipped-version argument that does not currently exist.

**A finding that reshapes this batch: the documented surface is 10 of 26
implemented.** `docs/18` lists 26 callable names; `api.cxx`'s `ENTRIES` table
registers ten (`tool.register`, `tool.unregister`, `model.register`, `on`, `off`,
`emit`, `log.*`). Absent: `cmd.register`, `defer`, `timer.*`, `notify`,
`cfg.get`, `session.snapshot`, `session.fork`, `spawn`, `net.*`, `fs.*`,
`skill.register`, `mcp.register`, `context.add_instructions`.

Two consequences the slices must respect:

- **A Lua extension can register and unregister a tool, subscribe to events,
  log, and register a provider. That is all.** So a first-party extension cannot
  read config (`cfg.get`), spawn a process, or touch the filesystem. Slice B's `skills`
  extension therefore depends on P2 — there is no alternative route to a skill
  body — and it must not assume `cfg.get` for its own configuration.
- **The manifest permission table gates functions that mostly do not exist.**
  `fs_read`, `fs_write` and `spawn` gate nothing, because `mcode.fs.*` and
  `mcode.spawn` are not implemented. `net` **is** enforced, but only where it
  already applies: `api.cxx` refuses `mcode.model.register` without it. So the
  boundary is load-bearing for `tool.register` and `model.register`, and
  decorative for the rest. Recorded here so the claim is not repeated as though
  the whole surface were live.

Completing the surface is **not** in scope. It is M5 work, and pulling it in
would triple this batch. The two additions above are the minimum the slices
need.

## File ownership, and the two files that would otherwise collide

Every file has exactly one writer. The audit that produced this table found two
collisions, both fixed rather than tolerated:

| Would collide | Fix |
|---|---|
| `cli/exec.{hxx,cxx}` — A's `--approval` / `--add-dir` flags, B's `mcode skill list\|validate` | `cli/exec.*` is **A's**. B puts its command in `cli/skill_command.{hxx,cxx}` and adds one dispatch branch to `main.cxx`, which is B's |
| `ext/api.cxx` — Phase 0's rows, B's skill bodies, C's mcp body | Phase 0 owns the table and creates `api_skill.*` / `api_mcp.*` with stubs; B and C implement their own file and never open the table |

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
