# Parallel Workstream 3 — The eight core tools

> **TL;DR:** Implement the eight core tools from `docs/06`, their schemas, the
> read-before-write invariant, result truncation with artifact spill, and the
> approval policy `bash` needs. Pure functions over `workspace` — **depends on
> nothing another branch is writing.**

Branch: `feat/core-tools`
Base: `master` **after Phase 0** (`docs/26` §Phase 0) — P1 and P2 land there
Owner: agent 3

---

## Why this slice

`main.cxx` registers eight tool *entries* whose only handler returns
`"<file contents for …>"`. The registry, the schemas, and the invariants are all
absent. `docs/06` calls the eight-tool table canonical and gives each tool's
behaviour per input class.

This slice is **the hands**: it reads, searches, edits, and executes. It touches
no loop state and no transport, which is what makes it independent.

---

## Scope

### In scope

| Item | Deliverable |
|---|---|
| Schemas | All 8, as JSON-Schema text, ≈2–2.5K tokens total |
| `read` | Offset/limit, per-class behaviour, binary refusal |
| `write` | Create/overwrite, 10 MiB cap, read-before-write |
| `edit` | Anchor-matched exact replacement, unified diff result |
| `glob` | Pattern matching over `workspace::glob` |
| `grep` | Regex content search, gitignore-aware, capped |
| `bash` | Exec with the approval policy, timeout, output caps |
| `ask_user` | Escalation; returns the answer |
| `tool_search` | BM25-class name/description match, two-stage disclosure |
| Read-before-write | `tools::session_reads` implementation + enforcement |
| Truncation | 2K inline / 25K hard, artifact spill, actionable notices |
| Errors | Structured `{ok, error, hint, retryable}` |
| Registration | `register_core_tools( registry, handlers, … )` |

### Explicitly NOT in scope

- **The loop** — workstream 2. Your tools are called by
  `agent_loop::execute`, which already exists.
- **Transport** — workstream 1.
- **OS-level sandbox enforcement (Landlock/seccomp, Windows restricted token).**
  Deferred deliberately: it is platform-specific and cannot be tested from this
  machine. What ships here is the **approval policy with a fail-closed default**,
  which `docs/16` M1 names as the boundary for Windows and macOS.
  Say so plainly in the PR.
- **`task`, `todo`, `git`, `github`, network tools** — bundled Lua extensions
  (`docs/23`), not core.

---

## The interface you implement (already on `master`)

`src/mcode/tools/session_reads.hxx` — implement `record`, `find`, `contains`,
`size`, `clear`. **Do not change the signatures**; workstream 2 reads this header.

---

## Tasks

### T0 — The write path does not exist

**Blocker, and the first thing to resolve.** `workspace` is **read-only**.
Verified against `fs/workspace.hxx`: `resolve`, `contains`, `glob`,
`read_viewport`, `read_file`, `content_hash`, `display_path` — no write, no
create, no remove. Nothing else in the tree writes a file either; the only
`ofstream` is in the eval harness, and `create_directories` appears twice, both
unrelated.

So `write`, `edit`, the artifact spill, and the "don't ask again" policy
persistence all have **no primitive to stand on**. You cannot write `write` on top
of an API that cannot write.

Add to `workspace`, as a Phase 0 change (see `docs/26` §Phase 0):

```cpp
enum class write_mode { create, overwrite };

struct write_receipt {
    std::string content_hash;   // the hash AFTER the write, for session_reads
    std::uintmax_t bytes_written = 0;
};

[[nodiscard]] auto write_file( std::string_view relative_path, std::string_view content,
    write_mode mode ) -> result< write_receipt >;
```

Requirements, each of which is a documented failure mode rather than a preference:

| Requirement | Why |
|---|---|
| `write_mode { create, overwrite }` explicit | The read-before-write invariant is enforced by the **caller**, and it needs to distinguish "new file" from "replacing content" — create needs no prior read, overwrite does |
| Path resolved through `resolve()` then checked with `contains()` | The workspace boundary is the security control (`docs/12`); a write path that skips it is the whole boundary gone |
| Atomic: temp file in the same directory, then rename | A crash mid-write must not leave a truncated source file. `docs/03` makes the event log crash-safe; a half-written `.cxx` is worse than a lost turn |
| Parent directories created only for `create` | Otherwise a typo'd path silently creates a tree |
| Returns the new `content_hash` | `session_reads` needs it to record the write, or the next `edit` in the same turn reads as stale |
| Refuses a path inside `.mcode/` or `.git/` | `docs/12` deny-write; see T8's actor-scoped rule |
| Long-path handling via `platform::to_extended_path` | `read_file` already does this; a write that does not will fail on the same deep path that reads fine |

**Ownership:** this is Phase 0 because two branches need it — you for `write`/
`edit`, and workstream 1 for nothing, but the artifact directory is created by
your spill helper. Land it before branching and both stay conflict-free.

**Acceptance:** a round-trip test — write, read back, hash matches; a write outside
the root is refused; a write to `.mcode/config.toml` is refused; an interrupted
write leaves the original file intact.

### T1 — The tool-schema table

Eight schemas as JSON-Schema text, authored **in one place** and registered
through `tool_registry::add`.

**The schema has to go somewhere, and today there is nowhere.** Verified:
`tool_def` is `{name, description, klass, source, owner, deferrable}` — no schema
field. `chat_request.tools` is `tool_spec{name, description, schema_json}`, and
nothing constructs one from a `tool_def`. This is fixed in Phase 0 (`docs/26`
§Phase 0): `tool_def` gains `schema_json`. You set it at registration; workstream
2 reads it to build the request. **Do not add a parallel field or a side table** —
the registry is the one place both branches read.

The Phase 0 change also fixes a live bug this exposes: `ext/api.cxx` renders a Lua
tool's schema and then drops it, so every extension tool is currently callable but
never advertised. A regression test belongs with the Phase 0 commit, not here.

| Tool | Class | Notes |
|---|---|---|
| `read` | `read` | `path`, `offset`, `limit` |
| `write` | `write` | `path`, `content` |
| `edit` | `write` | `path`, `old_string`, `new_string`, `replace_all` |
| `glob` | `read` | `pattern`, `max_results` |
| `grep` | `read` | `pattern`, `path`, `glob`, `max_matches` |
| `bash` | `exec` | `command`, `timeout_ms` |
| `ask_user` | `read` | `question`, `options` |
| `tool_search` | `read` | `query`, `expand` |

Authoring rules from `docs/06` §Tool authoring rules — these are **review
criteria**, not suggestions:

1. **Flatten parameter schemas.** Nested params measured **−47%**. Every
   parameter is top-level.
2. **Put defaults and bounds in each parameter's description**, and **example
   values** — full example *calls* measured harmful.
3. **Keep parameter names in sync with the implementation in the same commit.**
   A stale description actively reverses gains.
4. **De-overlap sibling tools** — `read` vs `grep` vs `glob` must not imply each
   other's prerequisites.
5. **Every error explicit, typed, actionable.**

**Budget:** ≈2–2.5K tokens for all eight. Measure it — count the characters and
divide by ~4, and put the number in the PR. `docs/01` caps core at 3K.

**Acceptance:** a test asserts each schema parses as JSON and has the required
keys. A malformed schema must be rejected at registration, not passed to the
model (`docs/06`: schemas are untrusted input).

### T2 — `read`

Per input class, from `docs/06`:

| Input | Behaviour |
|---|---|
| Text | Offset/limit, 1-based numbers, default window **100 lines** |
| Binary (NUL in first 8 KiB, or known binary extension) | **Refuse with a stub**: `{path, size, detected_type}` + hint (`use bash with xxd/strings`). Never emit binary |
| Very large (>1 MiB or >50k lines) | Serve the window; stub reports total size |
| Minified (longest line >5 KiB, or generated marker) | Window + a `generated` note; suggest the source file |
| Wrong encoding | Decode UTF-8 with replacement; report `encoding: non-utf8` — do not fail |
| Symlink | Follow, resolved against the workspace boundary |
| Missing | Structured error with the **closest existing path** (typo recovery) |

Use `workspace::read_viewport` and `workspace::read_file` — both exist and are
tested. `looks_binary` exists.

**Record the read** in `session_reads` with `workspace::content_hash`. This is
what makes `write` and `edit` safe.

**Traps, and one conflict the table hides**

- The 100-line default is `DEFAULT_READ_LINES` — do not redefine it.
- Binary refusal happens on **bytes**, before decoding. Decoding first and
  checking after is how binary reaches the context.
- "Closest existing path" needs a bounded search — do not walk the whole tree.
  Use the parent directory plus a small edit distance.
- **The "very large" row is not implementable through `read_file`.** Verified:
  `read_file` hard-fails above `MAX_TEXT_FILE_BYTES` (8 MiB) with
  `"file exceeds the 8388608-byte read cap"`. The table above says a >1 MiB file
  should still *serve a window*, and `write` refuses only above 10 MiB. So a 9 MiB
  file is one the model may legitimately edit but **cannot read at all**, and
  `read` would return a size error instead of a window.

  Resolve it explicitly, and state the resolution in the PR:
  - Read the first N bytes with a bounded streaming read rather than calling
    `read_file`, so the window is served regardless of total size; **or**
  - Extend `workspace` with a windowed reader in Phase 0 (`docs/26` §Phase 0).

  Either way the numbers must agree across the three tools: `read`'s
  window threshold, `read_file`'s hard cap, and `write`'s 10 MiB cap. Three caps
  that disagree are three bugs waiting for a file that lands between them.
- `read_viewport` returns `read_result` with `first_line`/`last_line`/`total_lines`
  and text already prefixed `"<n>\t"`. Use those fields for the truncation notice
  instead of recomputing line numbers.

### T3 — `write`

- Refuses files > **10 MiB**.
- **Refuses to overwrite a file it has not read this session** — the
  read-before-write invariant.
- A file that was read and then changed on disk → **stale**, refuse with the
  current hash and a hint to re-read.

The staleness check: compare `session_reads::find(path)` against a fresh
`workspace::content_hash(path)`. Mismatch = someone else changed it.

Creating a **new** file needs no prior read — there is nothing to invalidate.
Distinguish create from overwrite explicitly.

**Acceptance:** a test per branch: create (no read needed), overwrite after read,
overwrite without read (refused), overwrite after external change (refused as
stale).

### T4 — `edit`

Exact-anchor replacement, **never a silent whole-file rewrite**.

- `old_string` must appear **exactly once** unless `replace_all` is set. Zero
  matches and multiple matches are **different, actionable errors**:
  - zero → "not found; the file may have changed — re-read it"
  - multiple → "appears N times; include more context to disambiguate, or set
    replace_all"
- Requires a prior read of the file (same invariant as `write`).
- Requires the file to be **not stale**.
- Returns a **unified diff**, not the whole file (`docs/06`: "Edits return
  unified diffs, not whole files").
- Updates `session_reads` with the new hash, so a second edit in the same turn
  does not read as stale.

**Traps**
- Normalize line endings for the *comparison* but preserve them in the file.
  A CRLF file edited with an LF anchor silently matches nothing.
- An empty `old_string` is a create, not an edit — refuse it with a hint to use
  `write`.
- The diff must be bounded like any other result.

### T5 — `glob` and `grep`

**`glob`** — wrap `workspace::glob`. It already enforces the workspace boundary,
the `**` cycle guard, and `DEFAULT_GLOB_LIMIT`. Return **paths relative to the
workspace root** via `workspace::display_path`.

**But it is not ignore-aware.** Verified: `workspace.cxx` contains no `.gitignore`
handling and no dot-directory or `build/` filter — it walks the tree and only sets
`skip_permission_denied`. So `glob "**/*.o"` will descend `.git/` and `build/` and
burn the whole `DEFAULT_GLOB_LIMIT` (1000) on artifacts. `docs/06` says `glob` has
an "expansion budget" as an invariant.

Add a filter in **your** layer, not in `workspace` (it is read-only for you):

- Always skip `.git/` and `.mcode/` — the first is never interesting, the second is
  your own artifact directory (T8).
- Honour the root `.gitignore` at minimum, and say in the PR that full gitignore
  semantics (nested files, negation, `**` rules) are out of scope for this slice.
- Report how many paths the filter removed when the result is truncated, so a
  surprising empty result is explainable.

**`grep`** — regex content search:
- gitignore-aware: honour `.gitignore` at the root at minimum. A full gitignore
  implementation is out of scope; say so in the PR.
- **Return summarized hits, not raw matches** (`docs/06` §Tool authoring rules, measured +6pp).
  One line per match: `path:line: text`, with the match highlighted.
- Cap: **50 matches or 2K tokens**, then paginate. `docs/05` §Numeric guidance owns that number.
- Skip binary files (reuse `looks_binary`).
- Compile the regex once; a bad pattern is a **fatal, actionable** error naming
  the regex engine's complaint.

**Traps**
- `std::regex` is slow and can catastrophically backtrack. **Bound the input
  size per file** and skip files over the cap.
- A pattern matching every line of a large repo must not produce a 500K-token
  result. The cap is enforced while scanning, not after.

### T6 — `bash`

General exec, and the composition escape hatch.

- Runs through `run_process` (exists): `process_options` with `working_directory`
  = the workspace root, `scrub_environment = true`, `timeout`.
- Default timeout **60 s** (`process_options::timeout` already defaults to it);
  `timeout_ms` overrides, bounded to a sane maximum.
- **Output caps** already exist (`max_output_bytes`, 1 MiB) — surface
  `output_truncated` honestly in the result.
- Exit code is part of the result. A non-zero exit is **not** a tool error — it
  is a successful call whose output says the command failed. Getting this wrong
  makes the model treat `grep` finding nothing as a crash.
- `timed_out` is reported distinctly, with a hint to raise the timeout.

**The approval policy** — this is the piece `bash` needs and cannot skip:

```
permission decision = session policy(class, argv) ∩ extension manifest
                      — more restrictive wins
```

For this slice: a `permission_policy` interface with:
- an allowlist/denylist of parsed **argv patterns**
- **fail-closed default**: with no policy configured, `exec`-class calls are
  **denied** unless `--yolo` was passed
- on "don't ask again", persist the **exact parsed argv**, never a wildcard
  prefix (`docs/06` §Permission gating)
- a test-visible decision function, so policy is testable without a terminal

**Explicitly deferred and to be stated in the PR:** OS-level enforcement
(Landlock/seccomp on Linux, restricted token on Windows, Seatbelt on macOS). The
policy above is a gate, not a sandbox. `docs/16` M1 says exactly this for Windows
and macOS; Linux's Landlock is the follow-up slice.

**Trap:** `docs/06` §Idempotency rules — exec-class calls are **not assumed idempotent**. The
harness must never auto-retry them. Nothing in this slice may retry `bash`.

### T7 — `ask_user` and `tool_search`

**`ask_user`** — ambiguity escalation. `docs/06` makes this core because its
absence makes the agent **guess instead of asking**, and nothing surfaces that.

- Takes `question` and optional `options`.
- In `exec --json` (headless) there is no terminal: **fail with a clear,
  actionable error** naming the question, so the caller sees what was needed.
  Never fabricate an answer.
- With a terminal, prompt and return the answer.
- The answer goes into history as a tool result, attributed.

**`tool_search`** — the bootstrapping meta-tool:
- **BM25-class** name/description matching over the registry. **No embedding
  model, no dependency** (`docs/06` §Tool search).
- Returns **name + one-line description**; a second level (`expand`) returns the
  full schema. That is Anthropic's two-stage disclosure without their API.
- ≈100–200 tokens per matched tool instead of the whole catalog.
- **Deferral is cache-neutral**: discovered schemas are appended to *history*,
  never spliced into the tools array. Your function returns text; the loop
  decides where it goes. Do not mutate the registry.

### T8 — Result truncation and artifacts

Applies to **every** tool result.

| Rule | Value |
|---|---|
| Inline cap | **2K tokens** (~8K chars) |
| Absolute hard cap | **25K tokens** per result |
| Overflow | Spill to `.mcode/artifacts/`, return a preview + the path |

- Truncation notices must be **actionable**: name the filter, the smaller range,
  or the exact next call. **Never a bare ellipsis.**
- Beyond the hard cap, truncate even the preview, with a resume hint
  (`"... truncated; re-call with offset=200"`).
- **Evidence loss is the hardest perturbation** (`docs/06`: scored 0.142 vs 0.460
  for structural noise). **Prioritize not-dropping over prettiness.**
- **The artifact path is run-scoped.** `docs/01` and `docs/03` both put artifacts
  at `.mcode/artifacts/<run-id>/`, not a flat directory. Use the run id from the
  event log so two runs cannot collide, and so an artifact referenced by a
  replayed session still resolves.
- The artifact directory must be created lazily and **not** be listed by
  `glob`/`grep`. `.mcode/` is **not** in `.gitignore` today (verified) — so either
  add it there, or the artifacts your helper writes show up as untracked files in
  the user's `git status`. Adding `.mcode/` to `.gitignore` is the right call and
  is a one-line change; do it and say so in the PR.

One shared helper, used by all eight tools. Eight hand-rolled truncators is
eight behaviours.

#### The deny-write invariant, and why two actors differ

`docs/12` denies **the model's tools** any write to `.mcode/` or `.git/`
internals, and the project scope cannot override it. That denial is absolute:
a model that can write `.git/hooks/` or `.mcode/config.toml` has escaped the
trust boundary, and a model that can write `.mcode/artifacts/` can **forge
evidence** — plant a file, then cite it.

But the **harness** must write artifacts there, because that is where
`docs/03` §Persistence layout puts them. So the rule is actor-scoped, and the
two paths must not share a helper:

| Actor | May write `.mcode/artifacts/` | May write `.mcode/config.toml` | May write `.git/` |
|---|---|---|---|
| `write`/`edit` tools (model) | **no** | **no** | **no** |
| truncation spill (harness) | yes | no | no |

Enforce it in the workspace boundary check, which already owns `contains()`:
add a deny list consulted by the **tool** path only. Do not implement it by
making the spill helper a special case inside `write` — that is how a
later refactor hands the model the escape.

**Acceptance:** `write` to `.mcode/config.toml`, `.mcode/artifacts/x` and
`.git/hooks/pre-commit` are each refused, with a test per path; and the spill
helper still writes an artifact successfully.

### T9 — Error contract

Every tool failure returns the same shape:

```json
{ "ok": false, "error": "<what failed>", "hint": "<specific fix>", "retryable": bool }
```

- `docs/06`: **59% of "successful" MCP results contained errors with
  `isError=false`.** A model that cannot trust the success signal thrashes.
- Error text is prompt engineering: **write it like advice to a colleague,
  including the fix.**
- `retryable` must be honest. `bash` timeouts are retryable with a bigger
  timeout; a missing file is not.

### T10 — Registration

`src/main.cxx` today has `build_core_registry`, which adds the eight names with a
one-line description and no schema. **You replace its body** — the tool table
becomes the source of both the name and the schema, so there is one list rather
than two that drift.

```cpp
// tools/register.hxx
auto register_core_tools( tool_registry& registry, tool_handler_sink& sink,
    const tool_context& context ) -> status;
```

`tool_context` carries: the workspace, the session reads, the approval policy,
the artifact directory, and the config. **A request struct, not six positional
parameters.**

**Take a sink, not an `agent_loop&`.** `docs/03` §Dependency rule is explicit that
layers point down only, and `agent/loop.hxx` is *above* the tools layer. A
`tools/register.hxx` that includes `agent/loop.hxx` inverts the dependency and
makes the tools layer un-testable without the loop — which is precisely what your
own slice tests need to avoid.

`tool_handler_sink` is one method:

```cpp
struct tool_handler_sink {
    virtual ~tool_handler_sink( ) = default;
    virtual auto add_handler( std::string name, tool_handler handler ) -> void = 0;
};
```

`agent_loop` already has `register_handler( std::string, tool_handler )` with
exactly that signature (verified), so workstream 2 makes it implement the
interface — a one-line change, no behaviour moved. Your tests pass a stub. This is
the standard dependency-inversion move, and it is what keeps `tools/` free of
`agent/`.

`main.cxx`'s smoke test calls this, then `loop.register_handler` per tool. The
stub handlers (`"<file contents for …>"`) are deleted, not kept as a fallback —
a placeholder that returns plausible text is worse than a missing one, because it
reads as success.

**Acceptance:** the smoke test registers the real tools and `read` returns real
file content from a fixture.

---

## Tests

| Test | Asserts |
|---|---|
| `every schema parses and has required keys` | Registration validation |
| `a malformed schema is refused` | Untrusted input |
| `read returns numbered lines` | 1-based, window default 100 |
| `read refuses binary with a stub` | NUL byte → stub, no bytes |
| `read reports total lines when truncated` | Navigation |
| `read on a missing file suggests a close path` | Typo recovery |
| `read records the hash` | Feeds the invariant |
| `write creates a new file` | No prior read needed |
| `write refuses an unread overwrite` | Invariant |
| `write refuses a stale overwrite` | External change |
| `write refuses over 10 MiB` | Cap |
| `edit replaces a unique anchor` | Returns a diff |
| `edit refuses zero matches` | Actionable error |
| `edit refuses multiple matches without replace_all` | Actionable error |
| `edit preserves CRLF` | Line-ending trap |
| `edit refuses an empty old_string` | Create vs edit |
| `glob returns workspace-relative paths` | Boundary |
| `grep caps at 50 matches` | Cap enforced during the scan |
| `grep skips binary files` | Reuses looks_binary |
| `grep reports a bad regex actionably` | Error contract |
| `bash reports a non-zero exit as success` | Not a tool error |
| `bash reports a timeout distinctly` | `timed_out` |
| `bash denies exec by default` | Fail-closed |
| `bash allows with an argv pattern` | Policy |
| `ask_user fails clearly when headless` | Never fabricates |
| `tool_search returns names then expands` | Two-stage |
| `tool_search does not mutate the registry` | Cache-neutral |
| `an over-cap result spills to an artifact` | Path + preview |
| `the truncation notice names the next call` | Actionable |
| `every error has error, hint and retryable` | Contract |
| `core schemas are within 3K tokens` | Budget |
| `write refuses .mcode/config.toml` | Deny list |
| `write refuses .mcode/artifacts/x` | Forged evidence |
| `write refuses .git/hooks/pre-commit` | Trust boundary |
| `the spill helper still writes an artifact` | Harness is not the model |
| `write is atomic` | An interrupted write leaves the original intact |
| `write refuses a path outside the root` | Boundary |
| `write returns the new hash` | Feeds session_reads |
| `read serves a window past the 8 MiB read cap` | The T2 conflict, whichever way it is resolved |
| `glob skips .git and .mcode` | Expansion budget |
| `glob reports how many paths it filtered` | Explainable truncation |

---

## Acceptance criteria

1. All eight tools registered, callable, and returning real results.
2. The smoke test's `read` returns actual file content, not a placeholder.
3. Core schema budget measured and ≤ 3K tokens, reported in the PR.
4. Read-before-write enforced, with the four branches tested.
5. `bash` denies by default; the PR states plainly that OS-level sandboxing is
   deferred and the policy is a gate, not a sandbox.
6. `ctest` 100% pass; `_clgate.py` exit 0; smoke exit 0; bench gate pass.
7. Every new file ≤ 600 lines; no `docs/NN` citations; no magic numbers; trailing
   return types; spaced parens; `≤4` positional parameters.
8. `docs/06` updated if the implementation revealed a gap in the table — say
   which in the PR.

---

## Merge-conflict surface

| File | Other branch | Resolution |
|---|---|---|
| `src/CMakeLists.txt` | 1 and 2 | **Expected conflict.** Add sources in the same block |
| `src/main.cxx` | none | you replace the stub handlers |
| `src/mcode/tools/*` | none | you own the directory |
| `src/mcode/tools/session_reads.hxx` | none | **frozen — implement, do not edit** |
| `src/mcode/fs/workspace.*` | none | read-only; if it needs a fix, say so in the PR |

Do **not** touch `agent/loop.*`, `model/*`, `net/*`, `cli_commands.cxx`.

---

## Risks

| Risk | Mitigation |
|---|---|
| Shipping `bash` with no gate | The approval policy is fail-closed and tested. The PR states the deferral explicitly |
| 8 tools blowing the schema budget | Measured, asserted, and reported |
| `grep` producing a huge result | Cap enforced during the scan |
| `std::regex` catastrophic backtracking | Bound per-file input size |
| Truncation dropping evidence | One shared helper; the notice names the next call; the artifact path preserves the whole result |
| `edit` silently rewriting a whole file | Exact anchors only; multiple matches refuse |

## Sources

- `docs/06-tools.md` — the eight-tool table, per-input-class `read` behaviour, the
  authoring rules and their measured effect sizes, the error contract
- `docs/05-context-engineering.md` — result caps, the 100-line viewport, the
  artifact spill path
- `docs/12-security.md` — the permission table and the `.mcode/`/`.git/` deny-write
  rule
- `docs/03-architecture.md` §Persistence layout — where artifacts live
- `docs/16-roadmap.md` — what M1 gates and what is deferred to M6
- `docs/01-north-star.md` — the core tool-schema budget
