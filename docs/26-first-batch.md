# First Batch — Gate Decisions and the Measurement Spine

> TL;DR: The first batch is not M0 features. It is the four decisions that are expensive to reverse (extension VM, sandbox boundary, extension ABI, provider seam) plus the measurement spine that judges them. Everything else in `16` waits on these, because the VM choice is a build-time decision and the ABI is a forever commitment.

## Why this batch, and why now

`16` sequences M0–M8 by feature. That ordering assumes the extension VM is settled. It is not: `17` chose LuaJIT on three grounds — FFI, JIT throughput, and size — and the hard requirement ("extensions must not be able to do harm") forfeits the first two. A JIT-off, FFI-less LuaJIT is a different product than the one `17` evaluated.

Three facts drive the batch:

1. **The VM is a build-time decision.** FFI removal, allocator routing, and bytecode policy are compile flags and host code. Retrofitting them touches the recipe, the host, and every extension.
2. **The API surface is a permanent commitment.** `18` is explicit: every exposed name is forward-compatibility debt forever. Freezing it before the sandbox model exists means freezing it wrong.
3. **RAM efficiency is unmeasurable today.** The scaffold reports binary size and nothing else. Cold start, idle RSS, per-extension cost, and dispatch overhead have no harness, so every "efficient" claim in this batch would be unfalsifiable.

## Conflicts this batch must resolve

The VM change is not a swap. It **reverses two settled positions** and **invalidates three numbers**. None of this is optional cleanup; each is a doc that would otherwise assert something false.

| Doc | Current claim | Why it breaks |
|---|---|---|
| `01` principle 8 | "The extension VM is **not** a security boundary and is never described as one" | Adopting a designed boundary makes this false as stated. It must be rewritten to the honest form: *a designed, in-process capability boundary that reduces blast radius; not an OS sandbox and not formally proven* |
| `01` non-goal | "Not a sandbox implementation … it ships no sandbox of its own" | A VM-level boundary is not an OS sandbox, but the non-goal must say so explicitly or the contradiction is silent |
| `01` success criterion | "New provider adapter ≤ 300 LOC, one file" | Under D2/D3 a provider is a Lua descriptor plus zero C++ LOC. The criterion becomes "one file, zero recompilation" |
| `01` budget | "Extension load cost: O(1) in extension count" | Mathematically wrong — loading N extensions is O(N). The honest budget is *linear with a small constant*, and B2 supplies the constant |
| `12` §Layer 3 | Whole section asserts LuaJIT + FFI makes VM-level confinement a fiction | True of LuaJIT, not of the replacement. A3 rewrites it |
| `17` | "Why LuaJIT" | Becomes the record of a superseded evaluation, kept with a banner (`AGENTS.md` rule: correct in place, never delete a disproved claim) |
| `18` §What not to expose | Rows about FFI cost and `__gc` finalizer hazards | Both moot under the replacement; keep the rows marked as LuaJIT-era so they are not re-proposed |

## Decision 1 — Extension VM (the gate)

### Verified this session

| Finding | Evidence |
|---|---|
| `LUAJIT_DISABLE_FFI` is a supported build flag | `src/Makefile:95`, `lj_arch.h:618` → `LJ_HASFFI 0` |
| FFI is never registered, not merely hidden | `lib_init.c:33` guards `{ LUA_FFILIBNAME, luaopen_ffi }` behind `LJ_HASFFI` |
| The documented bytecode escape is closed by compile-time removal | `lj_bcread.c:239` compiles out the cdata-literal reader; `:405` **rejects** any `BCDUMP_F_FFI` chunk (`return 0`) |

That last row corrects `12` §Layer 3. Its claim — a bytecode chunk re-initializes FFI even if `ffi` was never registered — is true for *runtime* hiding (`ffi = nil`) and **false for compile-time removal**. LuaJIT-without-FFI is materially stronger than `12` assumes.

### What that still does not buy

Removing `ffi` removes arbitrary native execution. It does not make the VM memory-safe. `12` item 13 stands: parser/GC CVEs (CVE-2024-25176/77/78; Redis CVE-2025-49844, a 13-year-old parser UAF at CVSS 9.9). Untrusted *source* still reaches `luaL_loadbuffer`. Removing `ffi` also removes LuaJIT's 64-bit cdata arithmetic, which `17` lists as the workaround for the missing integer subtype.

### The alternative, from its primary source

`luau.org/sandbox` is written for exactly this requirement. Verbatim scope:

- `io.` and `package.` **removed entirely**; `os.` reduced to `clock/date/difftime/time`; `debug.` reduced to `traceback`/`info`; `dofile`/`loadfile` removed.
- **Bytecode access removed**: `loadstring` rejects bytecode, `string.dump` and `load` are gone. "Bytecode is hard to validate and using untrusted bytecode may lead to exploits."
- Globals, every library table, and the string metatable are **readonly via a VM feature unreachable from scripts** — assignments, `rawset`, and `setmetatable` all fail. The mechanism is `luaL_sandbox` on the state plus `luaL_sandboxthread` per script thread.
- **No `__gc`**: host-only tag-based destructors (`lua_newuserdatadtor`) that run before the memory block is freed. Deletes `18`'s finalizer hazard class outright.
- **Interrupts**: "any Luau code is guaranteed to call this handler eventually (in practice … at any function call or at any loop iteration)" — a host-side kill switch that does not depend on `jit.off()`, replacing the `LUA_MASKCOUNT`-under-JIT interlock `17` documents.
- Memory ceiling configurable from the host.
- Build reality: CMake, **no external dependencies beyond the STL/CRT**, MSVC 2017+/gcc-7+/clang-7+, MIT.
- Upstream's own caveat: "since the entire stack is implemented in C++, the sandboxing isn't formally proven."

### Recommendation

**Adopt Luau as the extension VM, conditional on A1's measurements.**

The two properties that made `17` choose LuaJIT are both already forfeit — the JIT is off by default, and `ffi` must go. What remains is size and Lua 5.1 familiarity, and neither outweighs a boundary that is designed, adversarially exercised at Roblox scale, and documented.

On RAM specifically, the comparison is **not** Luau versus LuaJIT — Luau ships a VM plus compiler and is plausibly larger. The comparison is Luau versus the only formally stronger option, a **separate extension process**, which costs a second address space, an IPC channel, and a supervisor. That is the VS Code extension-host model and the reason VS Code is heavy. In-process Luau is the lighter of those two, and A1 measures whether it is also lighter than LuaJIT.

### The honest limit, stated plainly

No in-process VM gives a guarantee. Luau's boundary is designed, tested, and documented; LuaJIT's does not exist. If the requirement is literally "cannot", only a process boundary satisfies it — and that is a different product with a different RAM profile. The batch therefore **defines the extension-host interface so a process tier is a later addition, not a redesign** (A4), and never describes Luau as a sandbox in user-facing text.

## Decision 2 — The provider seam (Lua adds models and gateways)

`15` assumes one C++ adapter file per provider; the requirement is that Lua adds gateways. These reconcile through the pattern `23` already establishes: **the subsystem is C++, the surface is Lua.**

The hot path cannot move to Lua — per-token delta parsing in a scripting VM is the one continuously measurable cost. So the provider descriptor is **declarative** and C++ applies it natively:

```lua
mcode.model.register({
  name     = "my-gateway",
  endpoint = "https://api.example.com/v1/chat/completions",
  auth     = { header = "Authorization", from = "env", name = "MY_GATEWAY_KEY" },
  stream   = {
    text_delta = "/choices/0/delta/content",
    tool_calls = { index = "/choices/0/delta/tool_calls/0/index",
                   args  = "/choices/0/delta/tool_calls/0/function/arguments" },
    finish     = "/choices/0/finish_reason",
    usage      = { in = "/usage/prompt_tokens", out = "/usage/completion_tokens" },
  },
})
```

C++ keeps HTTP, SSE framing, retry, the canonical taxonomy, egress policy, SSRF filtering, and caps (decision 38 preserved). It applies the JSON pointers with yyjson. Lua supplies the mapping. An `on_event` escape hatch covers exotic providers, opt-in, with its cost measured.

### Event-name gates (added building D3)

One wire format puts **two different things at the same pointer** and distinguishes them only by the SSE event name. [OI] Responses sends text as `response.output_text.delta` and tool arguments as `response.function_call_arguments.delta`, both at `/delta`. Applying the pointer unconditionally appends every text token to the tool-call arguments, which is a silent corruption rather than a visible failure.

So two optional descriptor fields gate the pointers by event name:

| Field | Gates | Absent means |
|---|---|---|
| `text_events` | `text_delta` | every event |
| `tool_call_events` | `tool_calls.*` | every event |

Empty is "every event", which is correct for a format that encodes meaning in the payload alone. A format that encodes it in the name declares the names.

### Bounds on wire-supplied values

Three values arrive from the gateway and are validated rather than trusted:

| Value | Bound | Why |
|---|---|---|
| Tool-call `index` | `[0, 256)` | It addresses an array and is stored as an `int`. A negative or huge value was a truncating cast followed by unbounded growth — ten bytes in, megabytes retained per stream |
| Pointer syntax | `/`-prefixed, no `/-`, no `*` | The applier resolves pointers literally. `/-` means "append to the array" and `*` is not a JSON pointer wildcard, so both resolved to nothing at first token |
| Descriptor keys | allowlisted per level | A typo such as `steam` for `stream` loaded with the field unset and surfaced as a first-token failure with no hint about the cause |

## The measurement spine

Nothing in this batch is judged without numbers. The spine is built first because it gates every later claim, and `16`'s own principle is "evals before claims".

| Metric | Target | Why it is here |
|---|---|---|
| Cold start | ≤ 15 ms | `01` budget; unmeasured |
| Idle RSS | ≤ 30 MB | `01` budget; unmeasured |
| Binary size | ≤ 25 MB | `01` budget; `14` has estimates, no measurement |
| **Extension load, per ext** | **≤ 1 ms** | New. "Loads fast" needs a number |
| **20 extensions loaded** | **≤ 10 ms** | New. Proves the constant in `01`'s load budget |
| **Allocated bytes per loaded ext** | **≤ 512 KB** | Revised by `28`: measured 344 KB mean / 376 KB peak. The original 64 KB came from A1's *shared-VM* probe; one VM per extension is ~313 KB of fixed cost, and the isolation is deliberate (`25b0dea`) |
| **Per-extension ceiling** | **8 MB default, enforced** | Unchanged. RAM efficiency is also containment |
| **Hook dispatch overhead** | **≤ 0.5 ms per vetoable event** | New. Synchronous hooks consume `01`'s ≤5 ms TTFT budget directly — a 3 ms hook chain blows it |

Allocator routing is the mechanism for the ceiling and for attribution: the host passes a custom allocator, counts per-extension bytes, and refuses past the limit. That is both the efficiency instrument and the memory-exhaustion control `12` lists as uncontainable in-process.

## Starting point

The scaffold's LuaJIT host was **replaced, not extended** (`A5`): the VM lifecycle, sandbox setup, allocator routing, and interrupt handling all changed. The rewrite is in `src/mcode/ext/lua_host.cxx` and carries a 29-probe boundary suite that runs as part of the `mcode` smoke test.

Two results from that work that the plan did not anticipate:

1. **Sealing is a one-way door.** Luau checks `readonly` on every C API write path, so `luaL_sandbox` permanently closes the host API surface. The surface must be complete before the first line of extension code runs. This is a stronger argument for `C1`'s frozen 22 entries than forward-compatibility was.
2. **Three Lua 5.1 shims survive** — `newproxy`, `setfenv`, `getfenv`. None is an escalation, but the smoke test asserts their presence rather than pretending they are gone.

## Tasks

Each task names its acceptance evidence. No task is done without a recorded measurement or a smoke run.

### Track A — Gate decisions

| # | Task | Acceptance |
|---|---|---|
| **A1** | **VM spike, all three platforms.** Build Luau (`Luau.VM` alone **and** `Luau.VM` + `Luau.Compiler`) and LuaJIT-with-`LUAJIT_DISABLE_FFI` on Windows/MSVC, Linux/GCC, macOS/Apple Clang. Measure static lib size, hello-world RSS, cold start, and a C-boundary-heavy microbenchmark. | A table of measured numbers per platform, reproducible from a repo script. **Pre-committed thresholds:** adopt Luau unless it adds > 1.5 MB over LuaJIT-no-FFI, **or** fails to build on any tier-1 platform, **or** exceeds 20 ms cold start for 10 extensions. Any breach ⇒ LuaJIT-no-FFI, with the weaker boundary recorded as accepted risk |
| **A2** | **Decide and freeze the VM.** Write the decision into `17` with A1's numbers; keep the LuaJIT evaluation with a superseded banner. | `17` rewritten; no doc still asserts LuaJIT as the choice |
| **A3** | **Rewrite the sandbox boundary statement** in `12` §Layer 3: exactly what the chosen VM does and does not contain, the residual engine-bug risk, the interrupt guarantee, and the ceiling. | A paragraph a reviewer can falsify |
| **A4** | **Define the extension-host interface** (in-process today, process tier later). | Interface sketch in `19`; no code |
| **A5** | **Package the VM.** Private Conan recipe or vendored tree with a CMake shim, pinned to a commit — the same treatment `17` mandated for LuaJIT. | `conan create` succeeds on all three platforms |
| **A6** | **Attribution.** Luau asks that user-facing product documentation carry attribution (MIT plus a request). Add it to `README.md` and the licence notice. | Attribution present; `docs/25` packaging note updated |
| **A7** | **Boundary conformance suite.** Escape attempts that must all fail: `io.*`, `os.execute`, `package.loadlib`, `loadstring` on bytecode, `rawset`/`setmetatable` on globals and the string metatable, `debug` reach, `collectgarbage` state mutation, unbounded `require`. Runs in CI. | Every attempt fails; the suite is the evidence for A3's claims, not the docs |

### Track B — Measurement spine

| # | Task | Acceptance |
|---|---|---|
| **B1** | **Benchmark harness**: cold start, idle RSS, binary size. Local + CI. | Machine-readable report |
| **B2** | **Extension load benchmark**: N extensions × registration-only `init.luau`. | Per-ext and aggregate load time; supplies `01`'s constant |
| **B3** | **Per-extension memory accounting** via the host allocator. | `mcode ext doctor` shows bytes per extension |
| **B4** | **Hook dispatch benchmark**: cost of a vetoable-event chain at 0/1/10/50 handlers. | Supplies the dispatch budget |
| **B5** | **Budget gates in CI**: fail on regression beyond tolerance (10% default, per metric). | A deliberate regression is caught |

### Track C — Extension ABI

| # | Task | Acceptance |
|---|---|---|
| **C1** | **Freeze API v1.** Reconcile `18`'s 22 entries against the VM's dialect (Luau is Lua 5.1-based, so `17`'s "LuaJIT 3.0 syntax backports" do **not** apply); drop what the VM cannot express safely. | `18` final; `mcode.api_version = 1` |
| **C2** | **Capability model.** Manifest permissions mapped to the exact host functions they gate, default-deny. Name the enforcement primitives (`luaL_sandbox`, `luaL_sandboxthread`, `lua_newuserdatadtor`). **Constraint: every C++ function exposed to Lua must be bounded in time**, because the VM interrupt cannot preempt inside a single long-running host call — an unbounded host function defeats the kill switch. | Every API entry has a permission class or is explicitly unprivileged, and every entry is bounded |
| **C3** | **Manifest schema** (`ext.toml`) frozen, including `api_version` and permission validation. | `19` final; a bad manifest fails the extension, not the session |
| **C4** | **Error and attribution conventions**: `value, err` vs `error(msg, 2)`, per-extension error counters, quarantine threshold chosen. Name the mechanism enforcing `18`'s 50 ms veto budget (VM interrupt, not a count hook). | Documented in `18`; threshold is a number, not a deferral |
| **C5** | **Author-facing API definition file** (`.d.luau`) for `mcode.*`, so `luau-analyze` type-checks and completes extension code. | An extension type-checks clean against it |

### Track D — Provider seam

| # | Task | Acceptance |
|---|---|---|
| **D1** | Canonical `ChatRequest`/`ChatEvent`/`Usage` in C++ (`15`'s structs). | Compiles; codec round-trip unit-tested |
| **D2** | **Declarative provider descriptor** + JSON-pointer delta applier. | A descriptor drives a real stream end-to-end |
| **D3** | **Dogfood: the reference provider ships as a Lua extension**, not a C++ file. | If it cannot be expressed, the descriptor is wrong |
| **D4** | `on_event` escape hatch, opt-in, cost measured. | Measured delta vs the declarative path |

`D1`–`D2` are VM-independent. **`D3`–`D4` require A2** — they are the first consumer of the frozen API.

### Track E — M0 skeleton

Per `16` M0, with two amendments: the provider client is Track D's seam rather than a C++ adapter, and the VM is created **lazily** (first extension load), never at boot.

| # | Task | Acceptance |
|---|---|---|
| **E1** | Platform shims: the seven OS seams (`24`), stub implementations. | Interfaces fixed on all three platforms |
| **E2** | Session event log: append-only JSONL, flush per event, replay. | Crash mid-run replays cleanly |
| **E3** | Event bus v1: closed tagged union, per-kind subscribers, sync dispatch. | `20` conformance |
| **E4** | Headless surface: `mcode exec --json`, stdout streaming, exit codes. | Exit codes match `22` |
| **E5** | Config loading: TOML, scopes, precedence. | `22` conformance |
| **E6** | Fixture repo + 10 deterministic smoke tasks. | Runs unattended |
| **E7** | Eval run-record schema + pass@k machinery. | Wired to E6 |
| **E8** | **Tool-registration dogfood**: one first-party tool in Lua, plus the `23` disable-all test. | Disabling every extension leaves a working, less capable agent |

E8 also closed the loader: `ext.toml` validation, the two-level namespace walk, `mcode.tool.register` / `mcode.model.register`, and the ownership rule that a registry entry always has a live VM behind it. See `18` §What E8 implemented and `19` §Loading lifecycle.

### Track F — Doc amendments

A2/A3 force edits to `01` (principle 8, the sandbox non-goal, two stale criteria, the load budget), `12` §Layer 3, `17`, `18`, `19`, `23`, `00-index`, `16`, `AGENTS.md`. These are tasks, not side effects: leaving a stale claim is the defect class `AGENTS.md` ranks first.

## Sequencing

```
B1 ──► A1 ──► A2 ──► A3 ──► A5 ──► A7 ─┬─► C1 ──► C2 ──► C3 ──► C4 ──► C5 ─┐
                                        │                                    │
D1 ──► D2 ──────────────────────────────┼────────► D3 ──► D4                │
                                        │                                    │
                                        └────────► E1 … E7 ──► E8 ◄──────────┘

B5 ──► (gates every later milestone)
B2, B3, B4 ──► harness written with B1; numbers require the loader (after C5)
A4 ──► after A2, anytime        A6 ──► with A5        F ──► with A2/A3
```

Two dependencies worth stating because they are easy to get wrong:

- **`D3`/`D4` require A2**, not just `D1`. The reference provider ships *as an extension*, so it is the first consumer of the frozen API.
- **`B2`/`B3`/`B4` produce no numbers until the loader exists** (after `C5`). The harness is written early so the numbers land the moment there is something to measure — but a benchmark that cannot run is not progress.

`A1` and `D1`–`D2` run in parallel. Nothing in Track E starts before `A2`, because the VM choice determines the build.

## The next batch, split three ways

This batch ends with the VM, the ABI, and the measurement spine settled — and an
`mcode exec` that prints `no model client in M0` and exits 4. The batch after it
is the agent core, and it runs as **three parallel workstreams on one base**
because the three slices have no code dependency on each other:

| Plan | Branch | Delivers | Depends on |
|---|---|---|---|
| `29` | `feat/model-client` | TLS, byte-stable request rendering, the streaming client, retry, credentials | nothing in this batch's output beyond the frozen `model_client` |
| `30` | `feat/turn-loop` | The `04` state machine, context assembly, compaction, thrash detection | the abstract `model_client`, which a scripted fake satisfies |
| `31` | `feat/core-tools` | The eight core tools, schemas, truncation, the approval policy | `workspace`/`registry`/`session_reads` only |

Two interfaces are frozen on `master` **before** the branches start —
`model::model_client` and `tools::session_reads`. That is what makes the three
branches conflict-free: an interface each branch invents is three interfaces, and
the merge is a rewrite. Both headers compile with only their implementations
missing.

The only shared files are `src/CMakeLists.txt` (three-way, mechanical) and
`src/cli_commands.cxx` (`29` adds a single-request path, `30` replaces `run_exec`'s
body; take `30`'s structure and keep `29`'s provider selection).

### Phase 0 — the shared-interface changes, landed before any branch

Reviewing the three plans against the code found four places where a branch needs
an interface that **does not exist**. Each is small, each is needed by more than
one branch, and each would otherwise be invented three times and merged as a
rewrite. They land on `master` as **Phase 0**, before `feat/*` is cut.

| # | Change | Why it is shared | Blocks |
|---|---|---|---|
| P1 | `tool_def` gains `schema_json` | The loop builds the request's `tools` array from the registry; `tool_search` expands a match back to its schema | 2, 3 |
| P2 | `workspace::write_file( path, content, write_mode ) -> result< write_receipt >` | `workspace` is read-only today: no write, no create. `write`, `edit`, the artifact spill and policy persistence have no primitive | 3 |
| P3 | `json::document` gains array/nested-object writing | Its mutable API is flat `set_string`/`set_int` only; a request body is nested objects containing arrays | 1 |
| P4 | `net/http_client` reports `{status, headers, body}` on a failed SSE response | Today the status is stringified and the headers and body are discarded — `Retry-After` and the 429 quota/rate-limit split are undecidable | 1 |

**P1 also fixes a live bug.** `ext/api.cxx` renders a Lua tool's schema into
`registered_tool::schema_json` and then drops it when building the `tool_def`.
Every extension tool is currently callable but never advertised, and no test
notices — the fixture's own `hello` tool has a schema that nothing can read. P1
carries the schema through and adds the regression test.

**Each of P1–P4 is a single-purpose commit with its own test**, on the branch that
needs it least so the diff stays reviewable. None of them is a design change: P1
and P3 extend an existing type additively, P2 adds a function to a class that
already owns the concept, and P4 adds an outcome to a failure path that currently
loses information.

**What Phase 0 deliberately does not do:** it does not add a capability/pricing
registry (workstream 1's T8), does not extend `request_spec` with body-shape
fields (T9), and does not add a permission engine. Those are branch work, and each
is a decision that belongs in a doc before it belongs in code.

### Scope divergence from `docs/16`, stated deliberately

`docs/16` splits this work across two milestones: **M1** ships *six* tools
(`read`, `edit`, `write`, `glob`, `grep`, `bash`) and **M2** owns the context
manager and compaction. These plans ship **eight** tools and the context manager in
one batch.

The divergence is intentional and it is recorded here rather than left implicit:

- `docs/06` names eight core tools as the canonical set, including `ask_user`
  (silent-failure: its absence makes the agent guess) and `tool_search` (the
  loading mechanism itself). M1's six-tool list is a subset, not a different
  decision.
- `ask_user` is **cheap and load-bearing**: it is one schema and one prompt. It
  has no dependency on the context manager. Deferring it costs a known failure
  mode for no saving.
- `tool_search` is included as a **registration and ranking** concern only. Its
  deferral behaviour — moving schemas behind the search — needs the cache-neutral
  append-to-history path, which *is* the context manager's. So the tool ships and
  the deferral stays off until M2's machinery exists.

**`docs/16` is not edited to match.** It sequences milestones by feature, these
plans sequence one batch by dependency, and the two are allowed to differ as long
as the difference is stated. If the batch succeeds, `docs/16` M1/M2 should be
re-cut to match what actually shipped; that is a follow-up, not a prerequisite.

### Who owns the object graph after the three merge

**This is the gap the split creates, and it has to be named before the branches
start.** Each branch proves its slice in isolation — and a slice test uses a fake
for whatever is not in the slice. So all three can be green while `run_exec`
still exits 4, because **nobody constructed the real objects together**:

```
provider_registry ──► resolve descriptor ──► resolve_api_key
        │
        ▼
http_model_client ◄── http_client ──► workspace ──► session_reads
        │                                              │
        ▼                                              ▼
    the loop (30) ◄──────── register_core_tools (31) ──┘
```

**`30` owns this function** — it is the branch that replaces `run_exec`'s body and
therefore has to build what the loop consumes:

```cpp
// cli_commands.cxx
auto run_exec( const exec_options& options ) -> exit_code;
```

Order: **Phase 0 → `29` → `31` → `30` → integration.** The integration is its own
commit, and its acceptance evidence is the only test that matters for the merge:
**`mcode exec "…"` against the loopback stub returns real file content from the
real `read` tool.** Not a state sequence, not a schema — an actual read.

`30` merges last because it owns `run_exec` and therefore needs both other slices
present to construct the graph. `31` before `30` because the loop dispatches into
the tool registry.

If that test is green, the three slices compose. If it is not, the failure is in
the wiring, and the wiring was never any single branch's acceptance criterion.
Budget it as its own task rather than assuming the merge is free.

## Exit criterion for the batch

The batch is done when: the VM decision is recorded with per-platform numbers; the API is frozen at v1 with a capability model and an author-facing definition file; a provider, a tool, and a hook are each implemented **in the extension language, not C++**; and `B1`–`B5` report every budget in `01` as a measured number with CI gates enforcing them. Per-milestone done rules from `16` §"Definition of done" apply on top.

## Out of scope, explicitly

- **Host-side security is not solved here.** `12` lists 19 residual risks; several are host-side (env scrubbing, deny-read list, egress proxy, UNC prompts) and remain M1/M6 work. A3 hardens the *extension* boundary only.
- No TUI (`13`), MCP (`07`), skills (`08`), memory (`09`), subagents (`10`).
- No registry, install, or lockfile (`25`) — declared irreversible, own milestone.
- No three-platform OS sandbox enforcement. `16` moved that to M6.

## Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Luau exceeds the size threshold | Decision reverses | A1's thresholds are pre-committed; fallback is LuaJIT-no-FFI with the weaker boundary recorded as accepted risk |
| Luau fails on a tier-1 platform | Decision collapses | A1 builds on all three before anything depends on it |
| Dialect migration invalidates `18`'s sketch | Rework of the frozen surface | C1 runs after A2, never before |
| "Efficient" stays unmeasured | Unfalsifiable claims — the defect `AGENTS.md` ranks first | B1–B5 gate the batch |
| Sandbox language overclaims | Security incident via false confidence | A3 states the limit; A7 is the falsifying evidence; A4 keeps the process tier reachable; Track F removes the "never described as one" contradiction honestly rather than silently |
| **A7 finds a real escape** | Decision reverses after adoption | The suite runs before anything is built on the VM, not after. A failure is a finding, not a setback — it is exactly what the suite exists to produce |
| Provider descriptor cannot express real providers | The Lua-gateway goal fails | D3 dogfoods the default provider as Lua |

## Open questions

- Does the interrupt mechanism alone replace an instruction budget, or are both needed? Measure interrupt latency.
- Is per-extension VM state still right under Luau, or does safeenv make a shared state sufficient? Changes `19`'s load cost.
- Ship source (reviewable, parse cost) or precompiled bytecode (fast, needs signing)? `Luau.VM` alone is viable without the compiler, so this is a real size lever — measure both in A1.
- Does `18`'s ~20-entry ceiling survive the provider addition, or does `mcode.model.*` need its own namespace?

## Sources

- `luajit` source tree, verified this session: `src/Makefile:95`, `src/lj_arch.h:618`, `src/lib_init.c:33`, `src/lj_bcread.c:239,405`
- https://luau.org/sandbox/ — library removals, bytecode removal, readonly globals, `__gc` removal, interrupt mechanism, memory limits, upstream caveat
- https://github.com/luau-lang/luau — CMake build, no external dependencies, MSVC 2017+/gcc-7+/clang-7+, `luau_compile`/`luau_load` split, `luaL_sandbox`/`luaL_sandboxthread`/`lua_newuserdatadtor`, MIT plus attribution request
- https://luajit.org/faq.html — process-level sandboxing stance, bytecode danger
- https://www.corsix.org/content/malicious-luajit-bytecode — the cdata-literal escape `12` cites
- https://raw.githubusercontent.com/microsoft/vscode-docs/main/api/advanced-topics/extension-host.md — the out-of-process cost model
- Docs `01`, `12`, `15`, `16`, `17`, `18`, `19`, `23`, `24`, `25`
