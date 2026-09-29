# Workstream B — Skills and the instruction chain

> TL;DR: Nothing in this tree reads a `SKILL.md` or an `AGENTS.md`. The prompt is
> eight hardcoded sections and stops. This slice adds the two *data* paths —
> project instructions (always on, closest-wins) and skills (index always on,
> body on demand) — so an agent working in a real repository knows that
> repository's conventions and procedures. No new runtime, no interpreter.

## Why this slice

`docs/08` calls these "the low-ceremony path" and the point is that they stay
low-ceremony: a skill is a directory with a Markdown file in it, and a user who
wants to teach mcode a procedure writes prose, not Lua. That is the "keep it
stupidly simple" half of the batch requirement — the other half is `mcp`, and
the two are deliberately different mechanisms for different jobs.

Measured against the tree:

| Claim | Reality |
|---|---|
| `AGENTS.md` discovery | Does not exist. No code reads one — the only `AGENTS` match under `src/` is a comment in `cli/exec.hxx:45` |
| Skill discovery | Does not exist |
| `skill_read` tool | Does not exist |
| Prompt sections 9–12 (`21`) | `build_system_prompt` emits sections 1–8 and returns. The environment block, instruction chain and skill index are absent. Recitation is different: the tail-message plumbing exists (`loop_context.cxx:76-85`) but `loop.cxx:98` always passes `.recitation = {}`, so nothing is ever emitted — the producer is missing, not the mechanism |
| `mcode.skill.*` API | In the frozen table, unimplemented |
| The Lua surface generally | **10 of 26 documented names exist.** An extension can register a tool, subscribe, log, and register a provider — and nothing else. No `fs.read`, no `cfg.get`, no `spawn` |
| Session-start context budget | `05` allows 8.5K tokens: prompt 1.5K + tools 3.5K + **instructions 2K + skill index 1.5K**. The last two are unallocated because nothing produces them |

So today the agent starts every session with no knowledge of the repository it is
standing in. For a coding agent that is the difference between useful and
generic, and it is the cheapest large win available.

## Scope

**In**

- `AGENTS.md` discovery: walk cwd → root, one file per directory
  (`AGENTS.md`, else `CLAUDE.md`, else `GEMINI.md`), plus user-global and
  bundled-org locations
- Chain assembly with **closest-wins**, stated in the system prompt
- Overflow limits (32 KiB/file, 64 KiB/chain) with the 2K-token chain budget as
  a *lint warning*, not a truncation
- Skill discovery across four roots, including extension directories
- YAML-frontmatter parsing and spec validation (`name`, `description`)
- The **skill index** in the system prompt: `name — description`, budgeted
- The `skill_read` tool as a **bundled Lua extension**, per `23`
- `mcode skill list` / `mcode skill validate` CLI

**Out**

- `mcode.context.add_instructions` (the Lua prompt-contribution path). It needs
  the prompt assembler to be a real budgeted pipeline, which is `19`/`M2` work.
  It is also **not implemented**, so there is nothing to build on
- Filesystem watching and `/reload` for skills. `08` says a detected change
  stages the new chain for the next session boundary, never a mid-session
  rewrite; that staging machinery is not needed to be useful
- Nested/subtree JIT loading of `AGENTS.md` on first file touch. Real, valuable
  in monorepos, and separable — it is an injection on a read event
- Import/expansion syntax (`@path`, `@-mention`). `08` explicitly defers it as a
  budget hazard
- `allowed-tools` pre-approval from skill frontmatter. It needs workstream A's
  engine to exist and be tested first
- Model-invoked skill *selection* beyond the index. The model reads the index
  and calls `skill_read`; no router, no embeddings

## Design

### The instruction chain

Discovery order, which is also context order:

```
<install>/AGENTS.md          org / managed
%APPDATA%\mcode\AGENTS.md    user-global
<repo-root>/AGENTS.md        root
…/subdir/AGENTS.md           each ancestor down to cwd
```

**Closest wins** — the last file in the chain overrides earlier ones on
contradiction, and the system prompt says so verbatim. `docs/08` is unambiguous
that concatenate-and-hope is the failure mode (Gemini has no conflict algorithm;
[CC]'s issue #6235 is users hitting exactly this), so the rule is stated rather
than implied.

One file per directory, first match wins: `AGENTS.md`, else `CLAUDE.md`, else
`GEMINI.md`. Reading all three from one directory would triple the budget for
the common case where they are copies of each other.

Two caps, and the difference matters:

- **Byte caps are safety limits**: 32 KiB per file, 64 KiB per chain. They exist
  so a pathological file cannot hang startup. On breach, the *broadest* file is
  truncated first (the org file, not the closest one) with a one-line pointer so
  the model can `read` the rest.
- **The 2K-token chain budget is a lint warning**, not a truncation. `08` and
  `21` both cite the finding (arXiv 2602.11988) that a bloated chain buys no
  success rate and costs >20% more inference; `05` owns the 2K number itself.
  But silently dropping a user's instructions to hit a token figure is worse
  than telling them the chain is fat. `mcode skill validate`
  and `config check` report it.

### Skill discovery

Four roots, in precedence order for name collisions (project > user > extension):

```
<repo>/.mcode/skills/<name>/SKILL.md
<repo>/**/.mcode/skills/<name>/SKILL.md     nested
%APPDATA%/mcode/skills/<name>/SKILL.md      user
<ext-dir>/skills/<name>/SKILL.md            shipped inside an extension
```

That fourth root is the "write skills into extensions" requirement: an extension
directory may carry a `skills/` subtree, and discovery treats it as a skill root.
An extension that wants to ship procedural knowledge drops Markdown next to its
`init.luau` and gets it discovered — no API call, no registration.

Read the first 4 KiB of each `SKILL.md` and parse only the frontmatter. Validate:

| Field | Rule | On violation |
|---|---|---|
| `name` | `^[a-z0-9]+(-[a-z0-9]+)*$`, ≤64, **equals the directory name** | skip + log |
| `description` | present, ≤1024 | skip + log |
| `disable-model-invocation` | optional bool | honoured: `/name` only, absent from the index |

**Invalid never crashes startup.** A bad skill is a skipped skill with a log
line and a `mcode skill list` entry, exactly as a bad extension manifest is a
failed extension and not a failed session (`19`).

Canonical-path dedupe, because a symlinked skill root otherwise indexes the same
skill twice and two identical index lines waste budget and confuse routing.

### The index, and why it is conditional

Section 11 of the prompt is one line per skill, `name — description` with the
description truncated for routing. That is the whole of progressive disclosure
that the harness enforces; the body is fetched by the model calling a tool.

**The index is emitted only when the `skill_read` tool is actually registered.**

`docs/08` and `docs/21` §Conditional assembly both require this, and the reason
is concrete: `21` names a prompt that references an absent tool as an
unsatisfiable rule (the interference class Arbiter detects statically, and which
`21` makes a build failure), and a rule that cannot be satisfied teaches the
model that instructions are negotiable. A disabled `skills` extension must therefore leave **no** index —
which means the check is `registry.find("skill_read") != nullptr`, the same
`has_tool` pattern the existing builder already uses for `read`/`edit`.

### Where `skill_read` lives, and the one surface amendment

`docs/23` is explicit that `skill_read` is an **extension**, not core, and it
fails `06`'s tests outright: it is not **guaranteed** (the agent works without
it), not **trustworthy** in the sense that matters (its content is prose the
model reads, not a result the harness must believe), not **invariant-bearing**,
and not a **silent-failure** risk (`06` §Core tool set is the canonical list;
`23`'s three-name summary is a condensation of it). So the tool ships as a
bundled Lua extension under `extensions/skills/`.

That creates a requirement the frozen API surface does not cover. The extension
must, given a skill name, obtain the body — and it cannot do that with
`mcode.fs.read` without re-implementing discovery and getting the outside-the-
workspace permission story wrong for user-scope skills.

**Amendment: `mcode.skill.read(name)` and `mcode.skill.list()` are added to the
`mcode.skill` namespace.**

There is no way around this one. The extension cannot read the file itself —
`mcode.fs.read` is not implemented, and a user-scope skill lives outside the
workspace so it would need the outside-workspace permission path even if it
were. The host must expose the read.

`docs/18` freezes the surface at 23 rows and says additions are the thing to
resist — but it also records the precedent for exactly this situation:
`mcode.model.register` was added after the initial freeze, and the doc's own
conclusion is that *"a freeze declared before the feature that needs it is
premature… freezing after `D2` would have produced 23 the first time."* The
provider seam needed an entry; the skill seam needs one too. `v1` has not
shipped, so nothing published depends on the count.

The namespace already exists in the table (`mcode.skill.register`), so this
completes a namespace rather than widening the root. `mcode.skill.register`
itself stays unimplemented this batch — filesystem discovery covers the
requirement, and a registration path with no consumer is the "proven need, not
probably useful" bar that `18` sets.

`skill_read` takes a **name**, never a path. It resolves against the discovered
index and reads that skill's file. It cannot be turned into an arbitrary-file
read, so it needs no boundary check and cannot be used to escape the workspace —
which is also why it does not depend on workstream A.

`skill.list` returns the same rows the index is built from, so the model can ask
"what skills exist" without the harness having pre-spent the index budget. It is
cheap and it is the honest answer to a question the model otherwise guesses at.

### Budget accounting

`05`'s session-start row — the authority for this number — allocates: prompt
1.5K + tools 3.5K + instruction chain 2K + skill index 1.5K = 8.5K. This slice
is the first to spend the last two, so it is the first that can break the
total. The assembler reports each section's measured
cost and the sum, and a test asserts the sum stays inside the budget with a
realistic chain plus 15 skills.

## Files

| Path | Owner | Change |
|---|---|---|
| `src/mcode/skills/frontmatter.{hxx,cxx}` | B | minimal YAML subset: scalars + bools, no nesting, no anchors |
| `src/mcode/skills/discovery.{hxx,cxx}` | B | root scan, validation, dedupe, collision precedence |
| `src/mcode/skills/index.{hxx,cxx}` | B | index text, description truncation, budget measurement |
| `src/mcode/instruct/chain.{hxx,cxx}` | B | `AGENTS.md` walk, concatenation, closest-wins, byte caps |
| `src/mcode/agent/loop_context.cxx` | B | sections 10 and 11, conditional on the tool |
| `src/mcode/ext/api_skill.{hxx,cxx}` | B | the bodies for `skill.read` / `skill.list`; the rows and stubs are Phase 0's |
| `src/mcode/cli/skill_command.{hxx,cxx}` | B | `mcode skill list\|validate`. **Not** `cli/exec.*`, which is A's |
| `src/main.cxx` | B | one dispatch branch for the `skill` subcommand |
| `extensions/skills/{ext.toml,init.luau}` | B | the `skill_read` tool |
| `extensions/mcode.d.luau` | B | type declarations for the two entries |
| `src/cli_commands.cxx` | **integration** | discover, then hand the index to the loop |

## Steps

1. Frontmatter parser + validation, unit-tested against the spec's good and bad
   examples. This is the piece most likely to be subtly wrong, and it is pure.
2. Skill discovery over the four roots, with dedupe and collision precedence.
3. Index text and budget measurement.
4. `AGENTS.md` chain discovery, concatenation, closest-wins, byte caps.
5. Inject sections 10 and 11 into `build_system_prompt`, both conditional.
6. The API amendment, the bundled `skills` extension, and the matching
   declarations in `extensions/mcode.d.luau` — the definition file is what
   `luau-analyze` checks author code against, so an entry missing from it is an
   entry authors cannot use.
7. `mcode skill list|validate`.

## Acceptance

- **A skill on disk reaches the model.** Drop `SKILL.md` into
  `<repo>/.mcode/skills/demo/`, run a turn, and the index line is in the
  request. The model calls `skill_read("demo")` and the body comes back.
- **`AGENTS.md` is in the prompt, closest-wins.** A root file and a nested file
  with contradictory instructions: the nested text appears later in the chain
  and the precedence rule is stated. This is the assertion that would catch
  concatenate-and-hope.
- **A disabled `skills` extension leaves no index.** Run with `--no-extensions`;
  assert the prompt contains no skill lines and no reference to `skill_read`.
  This is the dangling-reference class, and it is the single most likely
  regression.
- **An invalid skill is skipped, not fatal.** A `SKILL.md` whose `name` does not
  match its directory, next to a valid one: the valid one indexes, the invalid
  one is logged, the session starts.
- **The session-start budget holds.** With a realistic chain and 15 skills, the
  assembled request is inside 8.5K tokens.
- **A 40 KiB `AGENTS.md` does not hang startup** and is truncated broadest-first
  with a pointer.

- **`disable-model-invocation` hides a skill from the index but keeps `/name`
  working.** Asserted in both directions: absent from the prompt, reachable by
  the command.
- **Collision precedence is project > user > extension.** Three skills with the
  same name in three roots: the project one is the one that loads, and the other
  two do not produce a second index line.
- **A symlinked skill root does not double-index.** A root that resolves to a
  directory already scanned produces one entry, not two.
- **The user-global root is discovered.** A skill under
  `%APPDATA%\mcode\skills\` appears in the index for a repository that does
  not contain it — the case that proves discovery is not workspace-only.

Live evidence: run a turn in this repository, which has an `AGENTS.md`, and
confirm the agent follows a convention stated only there (e.g. `.cxx`/`.hxx`
extensions) without being told in the prompt.

## Traps

- **`name` must equal the directory name.** It is the routing key and the
  collision key; allowing a mismatch makes two skills answer to one name.
- **Frontmatter is not YAML.** Support scalars and bools and refuse the rest
  loudly. A half-YAML parser that silently mis-parses an anchor is worse than
  one that refuses.
- **Descriptions go into the system prompt.** Sanitize control characters and
  escape `<`/`>`; a community skill's description is attacker-controlled text in
  the most privileged position in the request (`08` §Traps).
- **The index is a dangling reference if the tool is absent.** Conditional on
  `registry.find("skill_read")`, never on a config flag.
- **Skill roots are outside the workspace.** `skill_read` takes a name and
  resolves internally, so it never becomes an arbitrary read and never needs a
  permission check. Do not "simplify" it into a path parameter.
- **Byte caps are not token budgets.** Truncating to hit a token number drops
  user instructions silently. Warn instead.
- **Dedupe by canonical path.** A symlinked root indexes everything twice.
- **The prompt is cache-prefixed.** Sections 10 and 11 are session-start only;
  nothing here may be recomputed per turn, or the cached prefix invalidates
  every request (`15`, `03`).

## Sources

- `docs/08-skills-and-agents-md.md` — the whole design: discovery order, closest-wins, the 2K/1.5K budgets, the index-only-when-the-tool-exists rule, the traps
- `docs/21-system-prompts.md` — sections 9–12, conditional assembly, the unsatisfiable-rule interference class
- `docs/23-first-party-extensions.md` — `skill_read` is an extension, and the shipping catalog
- `docs/16-roadmap.md:110` and `docs/19-extensions.md` — the disable-all test, which those two define
- `docs/18-lua-api.md` — the frozen surface, the `mcode.skill.register` row, and the `model.register` amendment precedent
- `docs/05-context-engineering.md` — the 8.5K session-start row (`:71`), which that doc marks as the authority, plus the numeric guidance
- `docs/01-north-star.md` — the hard budgets this batch must not regress (cold start, RSS, binary size, tool schema)
- `src/mcode/agent/loop_context.cxx:106` — `build_system_prompt`, and the existing `has_tool` conditional pattern
- https://agentskills.io/specification — frontmatter fields and validation rules
- https://agents.md/ — closest-wins nesting FAQ
- https://arxiv.org/html/2602.11988 — instruction bloat costs success rate and >20% inference
