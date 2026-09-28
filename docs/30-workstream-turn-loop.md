# Parallel Workstream 2 — The turn loop, context assembly, and compaction

> **TL;DR:** Turn `agent_loop` from "dispatch one tool call" into the ReAct state
> machine from `docs/04`, with the context assembler and compactor from `docs/05`
> feeding it. Driven in tests by a **scripted fake client**, so it does not wait
> on the model client branch.

Branch: `feat/turn-loop`
Base: `master` at the commit that lands this plan
Owner: agent 2

---

## Why this slice

`agent_loop::execute` dispatches exactly one `tool_call` and returns. There is no
state machine, no iteration, no context assembly, and no compaction. This is the
largest single gap between "infrastructure" and "an agent".

It is also the slice that **owns the request**: everything the model reads is
assembled here. That is why context assembly belongs in this branch and not
split out — the assembler's output is the loop's input, and separating them
across two branches means neither can be tested end to end.

---

## Scope

### In scope

| Item | Deliverable |
|---|---|
| State machine | The exact `docs/04` machine: 10 states, guarded transitions |
| Termination | The 5 ordered checks, before every model call |
| Thrash detection | `(tool_name, canonicalized_args)` hash, rolling window |
| Budget accounting | Fold `Usage` events; the three-axis cap |
| Cost-aware stop | The `<20%` remaining system note |
| Reflection | Guarded, capped, evidence-driven |
| Context assembly | Budgeted, frozen prefix order, cache breakpoints |
| System prompt | ~1200 words, named sections, assembled from the effective tool set |
| Compaction | 80% trigger, structured output, pinned facts |
| Tool-result clearing | Restorable stubs, `clear_at_least` |
| Todo recitation | From session state, not the extension |
| Loop tests | Scripted fake client drives every transition |

### Explicitly NOT in scope

- **Transport, TLS, retry** — workstream 1. You call `model_client::stream`.
- **Tool implementations** — workstream 3. You dispatch through
  `agent_loop::execute`, which already exists.
- **The verification gate's *command*** — M3. `Verify` here means "the model
  claims done and we look for a configured command"; with none configured, the
  transition is `Verify --no_tests--> Handoff`.
- **Session branching by lineage** — M3. Do not build it.
- **Subagents** — M8.

---

## Interfaces you build on (already on `master`)

```cpp
// model/client.hxx — abstract, so a scripted fake drives your tests
class model_client {
public:
    virtual auto stream( const stream_request&, const event_sink& ) -> status = 0;
};

// agent/loop.hxx — exists today
struct session_budget { std::uint32_t max_steps; std::uint64_t max_tokens; double max_usd; ... };
class agent_loop {
public:
    using tool_handler = std::function< result< std::string >( std::string_view args_json ) >;
    auto execute( const tool_call& call ) -> tool_outcome;      // keep
    auto register_handler( std::string name, tool_handler ) -> void;
};

// core/registry.hxx
class tool_registry { auto all() const -> std::vector< const tool_def* >; ... };
```

**`agent_loop::execute` stays as it is.** It is the dispatch boundary and it is
already tested; the loop calls it. Do not fold it into the state machine.

---

## Tasks

### T1 — The state machine

Implement exactly `docs/04` §The loop state machine. **Do not invent states.**

```
States: Idle, Plan, Act, Observe, Verify, Reflect, Replan, Handoff, Done, Failed

Idle    --user_task-->       Plan
Plan    --plan_written-->    Act          [guard: steps > reserve]
Plan    --budget_exhausted-> Failed
Act     --tool_calls-->      Observe
Act     --no_tool_calls-->   Verify
Observe --results_appended-> Act
Observe --thrash_detected--> Reflect      [guard: reflect_count < 2]
Observe --hard_error-->      Reflect
Reflect --diagnosis-->       Act
Reflect --same_failure_2x--> Replan
Replan  --new_plan-->        Act          [guard: replan_count < 2]
Replan  --no_new_plan-->     Handoff
Verify  --tests_pass-->      Done
Verify  --tests_fail-->      Reflect
Verify  --no_tests-->        Handoff
Any     --B exhausted-->     Handoff
```

Implementation: **one `enum class` and one `switch` in one function.** No
framework, no vtable, no coroutine. `docs/04` says ~300 lines and that is the
target.

`Failed` is reachable **only** from `Plan --budget_exhausted`. Budget exhaustion
anywhere else lands in `Handoff` — the doc is explicit that a budget stop emits
partial state rather than reporting failure.

**Acceptance:** every transition has a test that drives it. A transition with no
test is a transition that will silently break.

### T2 — Termination, checked before every model call

In this order:

1. `Done` or `Handoff` reached
2. `budget.steps == 0`
3. `budget.usd <= 0` or `budget.tokens <= 0`
4. user interrupt
5. provider hard error after the retry policy is exhausted

Every terminal state emits a **structured summary**: goal, actions taken, last
failure, remaining budget. Write it to the log as a `run.end`-adjacent event and
return it to the caller.

**Trap:** check *before* the call, not after. A check after means one extra
request is always spent.

### T3 — Thrash detection

```
hash = ( tool_name, canonicalized_args )
window = 12 calls, repeat_limit = 3
```

- **Canonicalize args before hashing** — parse to JSON and re-serialize with
  sorted keys. Hashing raw argument text makes two identical calls hash
  differently when only key order differs.
- Escalation is **monotonic on repeat count**: `2× limit` → Replan,
  `3×` → Handoff. Not a ladder of hand-picked numbers.
- Also escalate on **no progress**: workspace content unchanged across
  `2 × repeat_limit` Act rounds, or the same test-failure signature at
  `repeat_limit`.
- On trigger, **inject the repeated calls as evidence** into the Reflect prompt.

Both constants are `inline constexpr` and named. `docs/04` calls them
calibration, not law — put them in one place with a comment saying so.

**Acceptance:** three identical calls trigger Reflect; three calls differing only
in key order also trigger it (that is the canonicalization test).

### T4 — Budget accounting

Usage arrives as `chat_event::kind::usage`. Fold with the existing `usage::add`
(max, not sum — correct for both delta and cumulative providers).

`session_budget::charge( tokens, usd )` already increments steps; **check the
existing signature before adding a second charge path.** Steps, tokens and USD
are three axes against one struct.

Cost comes from `compute_cost( caps, usage )` — provider-reported usage only.
**Never estimate tokens with a tokenizer.**

### T5 — Cost-aware stopping

When `budget.nearly_exhausted()` (already implemented: `<20%` of USD), inject a
system note into the next request: *"budget nearly exhausted — wrap up or report
blockers."*

This converts budget death from silent truncation into a cooperative stop.
`docs/04` is explicit that a resumable `Handoff` beats a burned budget.

**Trap:** the note is **volatile tail**, never the frozen prefix. Putting it in
the system prompt invalidates the cache from token 0.

### T6 — Reflection

`Reflect` = **one** model call with `{last failure, relevant diff, prior
reflections}`. Output appended to the log as a `Note`.

Guards, all three required before reflecting (`docs/04` §When to reflect):
1. a mechanical signal exists (failing test, compile error, nonzero exit)
2. the failure is **new** (not already in this run's reflection memory)
3. budget remaining > cost of one reflection + one corrective step

Caps: **max 2 per failure class, 4 per run.** Never on success.

**Trap:** do not reflect on the same failure twice — `docs/04` says that means
reflection already failed, so escalate to Replan instead.

### T7 — Context assembly

This is the piece that decides what the model reads. Follow `docs/05` exactly.

**Frozen prefix order: `tools → system → messages`.** Never inject anything
mutable before or into tools/system.

Session-start budget (`docs/05` §Context budget, the authority):

| Part | Budget |
|---|---|
| system prompt | ≤ 1.5K tokens |
| tools | ≤ 3.5K tokens |
| instruction chain | ≤ 2K tokens |
| skill index | ≤ 1.5K tokens |
| **total** | **≤ 8.5K tokens** |

Rules:
- **No timestamps, no cwd, no git state, no session id** in the prefix. Those go
  in the volatile tail.
- **Append-only history.** Never mutate an emitted message. Clearing and
  compaction are the only sanctioned rewrites, and each costs a full cache
  re-write.
- **Deterministic serialization** — `json::document::dump()` already sorts keys.
- Tool schemas sorted by name, once, at session start.
- **Tool budget:** core ≈2–2.5K tokens. Above 4K total, tier-2/3 tools move
  behind `tool_search` — but `tool_search` itself is workstream 3, so for now
  just compute the number and log it.

**Model tiering per state** (`docs/15` §Model-tier policy, §Reasoning-effort
scheduling): `Plan`/`Replan` and `Verify`/`Reflect` use the `plan` tier at high
effort; `Act` uses `act` at medium; the compaction summarizer uses `side` at
low. The three tier names come from `model.tier.plan`/`.act`/`.side` in config
(`docs/22`). **Routing is by call site, not by token count** — you tag each call.
Never auto-downgrade mid-task.

Cache breakpoints (`docs/05:106`): one at end-of-tools+system, one at the newest
message boundary. **You compute them** and write byte offsets into
`cache_plan::breakpoints`; **workstream 1 renders them** (see `29` §T2). Set
`cache.mode` correctly: `explicit_markers` only for a provider that needs
them, `implicit` when it caches on its own, `none` otherwise. Getting the mode
wrong either sends useless markers or loses the cache entirely.
`cache_plan` already exists in `chat_request`.

**Trap:** the tool array is **frozen for the session**. Adding or removing a tool
mid-session invalidates the cache and `docs/05:24` marks it "doesn't work".

### T8 — System prompt

~1200 words core, **excluding tool descriptions and the instruction chain**
(`docs/21`). Named sections in order; no timestamps, cwd, git state or
session id in sections 1–8 (`docs/21:46`).

The prompt is assembled from the **effective tool set**: a rule referencing an
unloaded tool is **omitted, never dangling** (`docs/00` decision 47).

Instruction-count lint: warn at 150, fail at 200 (`docs/21:123`). The evidence is
that reasoning models are near-perfect to 100–250 instructions then decline
steeply.

**Do not invent prompt content.** `docs/21` owns it. If the doc does not specify
a section, ask rather than writing prose — an unsourced prompt rule is exactly
the kind of claim `AGENTS.md` forbids.

**Acceptance:** word count ≤ 1200 core; a test asserts no timestamp/cwd/session
id appears in sections 1–8.

### T9 — Compaction

Trigger: **80% of the usable window.** Usable window = model context window −
reserved output (−16K) − 5% safety margin.

Output structure (`docs/05:54-55`):

| Keep | Rule |
|---|---|
| First 2–4 events | verbatim |
| Last **3 turns or 20K tokens**, whichever smaller | verbatim |
| Always pinned | user's original task text, current plan/todo, list of modified files |
| Post-compact file re-read | ≤5 most recently **modified**, each capped 5K tokens; larger → path reference |

Summarizer: the **`side` tier**, separate system prompt. The prompt must demand
**paths, symbols, error messages, unresolved questions, next steps** — a generic
summary loses exactly the tokens a coding agent needs.

**Traps**
- **Budget in tokens, not events.** 120 events can be 5K or 500K tokens.
- **Do not compact away errors.** `docs/05:136`: the model repeats failed actions
  without the evidence.
- Failed tool calls are kept until the task phase completes, then clearable.
- Compaction is **last**, after prevention → clearing → isolation.

### T10 — Tool-result clearing

Restorable stubs. `clear_at_least ≥ 20%` (`docs/05:101`) — a clear smaller than
that costs a full cache re-write without buying enough room.

Cleared results must be **restorable**: the stub carries enough to re-read the
content (path + range, or an artifact path).

### T11 — Todo recitation

Reads **session state**, not the extension (`docs/04`): with no plan stored it
degrades to restating the user's goal. This is why it survives the extension
being disabled.

The plan is a structured field on the session. `mcode::todo` extension does not
exist yet, so v1 = restate the user's goal.

---

## Tests

**Drive everything with a scripted fake client.** That is the point of the
abstract interface, and it is what makes this branch independent:

```cpp
class scripted_client final : public model_client {
    // Returns a queued sequence of events per call; records the requests it saw.
};
```

| Test | Asserts |
|---|---|
| `a plain turn reaches Done` | Text, no tool calls, `Verify --no_tests--> Handoff` |
| `a tool call is dispatched and observed` | Act → Observe → Act |
| `thrash triggers Reflect` | 3 identical calls |
| `key-order-only differences still thrash` | Canonicalization |
| `Reflect escalates to Replan at 2× limit` | Monotonic escalation |
| `budget exhaustion lands in Handoff, not Failed` | The doc's rule |
| `Failed is reachable only from Plan` | Drive Plan with no budget |
| `the summary names goal, actions, last failure, budget` | Structured terminal summary |
| `termination is checked before the call` | Call count, not just final state |
| `the near-budget note is in the tail` | Prefix bytes unchanged between turns |
| `the prefix is byte-stable across turns` | Two turns, same prefix bytes |
| `assembly stays within 8.5K` | Budget arithmetic |
| `compaction triggers at 80%` | Threshold |
| `compaction pins the task and the file list` | Structure |
| `compaction keeps errors` | The documented trap |
| `clearing is restorable` | Clear then re-read |
| `no timestamp in sections 1-8` | Regex over the assembled prompt |

**A byte-stability test is mandatory**: assemble twice, compare the prefix bytes.
That single assertion protects the cache contract, which is where the cost is.

---

## Acceptance criteria

1. A scripted conversation reaches `Done`, and one reaches `Handoff`, both
   asserted on the state sequence, not just the end state.
2. Every one of the 10 states is visited by at least one test.
3. Prefix byte-stability asserted.
4. **You own `run_exec`'s body, and therefore the object graph after the merge**
   (`docs/26` §Who owns the object graph). Construct the registry, workspace,
   session reads, client and loop in that order. Your own tests use a scripted
   fake and a stub registry — that is correct and it is why the integration is a
   separate step, not a gap in your tests.
5. `ctest` 100% pass; `_clgate.py` exit 0; smoke exit 0; bench gate pass.
6. Every new file ≤ 600 lines; no `docs/NN` citations; no magic numbers; trailing
   return types; spaced parens; `≤4` positional parameters (use a request struct).
7. `docs/04` and `docs/05` updated where the implementation revealed the doc was
   wrong — say which in the PR.
8. The state machine is **one function**. If it needs a framework, the design is
   wrong.

---

## Merge-conflict surface

| File | Other branch | Resolution |
|---|---|---|
| `src/CMakeLists.txt` | 1 and 3 | **Expected conflict.** Add sources in the same block |
| `src/cli_commands.cxx` | 1 (adds a single-request path) | **Expected conflict.** Yours replaces `run_exec`'s body; keep branch 1's provider selection and call your loop |
| `src/mcode/agent/loop.hxx` | none | you own it |
| `src/mcode/model/client.hxx` | none | **frozen — do not edit.** If a signature is wrong, say so in the PR |
| `src/mcode/tools/session_reads.hxx` | 3 | frozen; 3 implements it |

Do **not** touch `model/types.*`, `model/provider.*`, `model/delta_applier.*`,
`net/*`, or `fs/workspace.*`.

---

## Risks

| Risk | Mitigation |
|---|---|
| Inventing states or transitions | The doc has an exact machine. Copy it. Every state gets a test |
| The prompt becoming unsourced prose | `docs/21` owns the content. Ask rather than invent |
| Cache invalidation from a mutable prefix | The byte-stability test is the guard |
| Scope creep into verification or branching | Both are explicitly out of scope. `Verify` with no configured command is `Handoff` |
| Thrash constants tuned to nothing | Named, in one place, commented as calibration for the eval suite |

## Sources

- `docs/04-agent-loop.md` — the state machine, termination order, thrash
  constants, reflection policy, budget semantics
- `docs/05-context-engineering.md` — every context budget, the compaction
  structure, `clear_at_least`, the frozen prefix order
- `docs/21-system-prompts.md` — the prompt budget, cache-stable section order,
  the instruction-count lint
- `docs/15-model-layer.md` — tiering and reasoning-effort scheduling per phase
- `docs/22-config-and-cli.md` — `[agent]`, `[context]`, and `[model].tier.*`
- `docs/01-north-star.md` — vocabulary, and the session-start context budget
