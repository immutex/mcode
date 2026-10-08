# Agent Loop Design

> TL;DR: Ship a ReAct-style interleaved loop as the default, bolt plan/reflect in as guarded states, cap every run with a three-axis budget (steps, tokens, USD), and detect thrash via repeated-tool-call hashing before the model burns its budget.

## State of the field

| System / paper | Loop shape | Headline result | Cost note |
|---|---|---|---|
| ReAct [arXiv:2210.03629](https://arxiv.org/abs/2210.03629) | Thought→Act→Observe per step | ALFWorld 71% vs 45% act-only; WebShop 40% vs 30.1% act-only; HotpotQA ReAct alone 27.4 EM *below* CoT 29.4 — ReAct+CoT 35.1 | 1 LLM call per step |
| Self-Consistency [arXiv:2203.11171](https://arxiv.org/abs/2203.11171) | N parallel CoT chains + majority vote | GSM8K 56.5→74.4 (PaLM-540B, 40 paths) | ~40× generation cost |
| Reflexion [arXiv:2303.11366](https://arxiv.org/abs/2303.11366) | Trial loop + verbal memory | HumanEval 80.1→91.0; ALFWorld 130/134 | Multiple episodes; needs reliable evaluator (MBPP false-pos 16% hurt it) |
| Self-Refine [arXiv:2303.17651](https://arxiv.org/abs/2303.17651) | Generate→self-critique→revise | Big gains on preference tasks (dialogue 25.4→74.6 GPT-4); ~0 on math | Feedback quality ablation: generic feedback ≈ no feedback |
| Self-correction critique [arXiv:2310.01798](https://arxiv.org/abs/2310.01798) | — | Intrinsic self-correction (no external signal) *degrades* reasoning accuracy; earlier gains traced to oracle labels | ICLR 2024 |
| Tree of Thoughts [arXiv:2305.10601](https://arxiv.org/abs/2305.10601) | Branch/evaluate/prune/backtrack | Game of 24: CoT 4% → ToT 74% (GPT-4); mini-crosswords needs both pruning (60%→41.5% without) and backtracking (60%→20% without) | ~5–100× CoT tokens; $0.74 vs $0.47 (CoT best-of-100) on Game of 24 |
| SWE-agent ACI [arXiv:2405.15793](https://arxiv.org/abs/2405.15793) | ReAct + purpose-built interface | SWE-bench full 12.47%; Lite 18.0% vs 11.0% shell-only baseline | Interface ablations: no edit tool 10.3%, no lint guardrail 15.0%, full history 15.0% vs last-5-obs 18.0% |
| Agentless [arXiv:2407.01489](https://arxiv.org/abs/2407.01489) | Fixed 3-phase pipeline, no agent | SWE-bench Lite 32.0% at $0.70/issue (GPT-4 Turbo); Verified 38.8% (194/500, GPT-4o) at ~$0.34/issue ([FSE'25](https://lingming.cs.illinois.edu/publications/fse2025.pdf)) | Beat contemporaneous agentic systems at the time |
| OpenHands [arXiv:2407.16741](https://arxiv.org/abs/2407.16741) | Event-stream runtime, CodeAct agent | 15 benchmarks; state = event stream + accumulated cost + delegation metadata | Append-only events, Docker sandbox |
| CodeMonkeys [arXiv:2501.14723](https://arxiv.org/abs/2501.14723) | Parallel trajectories × serial edit-test iterations + test-based voting + selection judge | SWE-bench Verified 57.4% (~$2,300 total); 66.2% selecting over ensemble of top submissions | Selection > best single member |
| Large Language Monkeys [arXiv:2407.21787](https://arxiv.org/abs/2407.21787) | Repeated sampling only | Coverage scales log-linearly with samples; 56% SWE-bench Lite w/ 300 samples (DeepSeek-Coder-V2) | Selection becomes the bottleneck |
| DeepSeek-R1 [arXiv:2501.12948](https://arxiv.org/abs/2501.12948) | RL-trained reasoning; loop learns self-verification internally | Emergent reflection/verification from verifiable rewards only | Training-time, not harness-time |
| s1 budget forcing [arXiv:2501.19393](https://arxiv.org/abs/2501.19393) | Force-continue thinking ("Wait") | AIME24 50.0→56.7%; gains flatten ~6× compute; over-forcing causes repetition loops | 1,000 SFT examples |
| ReWOO [arXiv:2305.18323](https://arxiv.org/abs/2305.18323) | Plan all tool calls up-front, execute, then reason once | 5× token efficiency, +4% HotpotQA; robust to tool failure | Brittle when step k+1 depends on step k's result |
| PRM verifier [arXiv:2305.20050](https://arxiv.org/abs/2305.20050) | Process reward model as best-of-N selector | MATH subset 78.2% (best-of-1860) vs ORM 72.4% vs majority vote 69.6% | PRM800K: 800K human step labels |
| Verifier overoptimization [arXiv:2210.10760](https://arxiv.org/abs/2210.10760) | — | Gold reward peaks then declines under best-of-N optimization against imperfect proxy (Goodhart) | Applies directly to harness-side verifiers |
| mini-SWE-agent ([GitHub](https://github.com/swe-agent/mini-swe-agent), [swebench.com](https://www.swebench.com/)) | ~100 lines, bash-only, no ACI | 65% SWE-bench Verified (Jul 2025) | Radical simplicity; now the leaderboard default harness |

### Agentless vs agentic — the actual tradeoff

- Agentless (2024-07): 32.0% Lite @ $0.70 vs SWE-agent (2024-03): 18.0% Lite @ ~$1.59 with GPT-4 Turbo. The *fixed pipeline beat the agent on both axes* at the time. Agentless's localization→repair→validate phases each use plain LLM calls with structured output — no tool loop at all.
- But the frontier moved: mini-SWE-agent (100 lines, pure ReAct + bash) hits 65% Verified; CodeMonkeys (agentic trajectories + selection) hits 57.4% with heavy parallel compute. The 2024 lesson "simple beats agentic" became "simple *harness* + strong model + selection beats complex harness."
- Resolution for mcode: the loop should be a thin, dumb ReAct core (mini-SWE-agent lesson), with sophistication moved into *selection* (CodeMonkeys lesson) rather than into loop complexity. A deterministic pipeline mode (Agentless-style localize→repair→verify) is a profile, not a separate architecture.

### Loop archetypes compared

| Archetype | Strength | Failure mode | Verdict for coding agents |
|---|---|---|---|
| Plan-then-execute (ReWOO, Agentless) | Cheap, predictable, parallelizable tool calls | Brittle to wrong early plans; can't use observations to replan | Good as a *profile* for well-scoped tasks |
| ReAct interleaved | Error recovery via observations; interpretable | Token-hungry; thrash-prone without guards | Default core |
| Interleaved plan/act (re-plan every k steps) | Middle ground | Re-planning churn can undo progress | Use: plan once, re-plan only on guard trigger |
| Event-driven (OpenHands) | Pause/resume, audit, multi-agent, streaming | More machinery; state persistence complexity | Adopt the *event log*, keep the loop simple |
| Tree search (ToT) | Massive wins when intermediate states are verifiable | 5–100× cost; needs a state evaluator | Only for optional best-of-N verify phase, not the main loop |

## What works / what doesn't

**Works**
- Interleaved act/observe with environment feedback (ReAct: +34pp ALFWorld over IL baseline).
- External, mechanical feedback: compile errors, test results, lint. Reflexion's HumanEval gains (+10.9pp) came with a 1% test false-positive rate; its MBPP regression (-3.0pp) came with 16%.
- Selection over candidates: CodeMonkeys voting+judge lifted 57.4%; ensemble selection 66.2% beat every member.
- Compact observations: SWE-agent last-5-obs beat full history by 3pp; summarized search beat exhaustive search by 6pp (18.0 vs 12.0 iterative-search ablation).
- Cost caps: Claude Code exposes `max_turns` and `max_budget_usd` with `error_max_turns`/`error_max_budget_usd` stop reasons ([docs](https://code.claude.com/docs/en/agent-sdk/agent-loop)).

**Doesn't**
- Intrinsic self-correction: asking the model to "check your answer" with no new information degrades accuracy (arXiv:2310.01798). Self-Refine's own ablation agrees: generic feedback ≈ no feedback (code optimization 26.0 vs 24.8 baseline).
- Unbounded reflection: Reflexion stopped improving after ~trials 6–7 on ALFWorld baseline; s1 shows over-forcing thought produces repetition loops.
- Faith in self-generated tests: 16% false-positive rate (Reflexion/MBPP) makes accept/reject decisions worthless.
- Optimizing hard against an LLM-judge: gold reward peaks then declines (arXiv:2210.10760); a verifier used for best-of-N needs a KL-like pressure or a hard cap on selection rounds.

### When to reflect vs when reflection hurts

Reflect (spend a dedicated LLM call on diagnosis) only when ALL hold:
1. A mechanical signal exists (failing test, compile error, nonzero exit) — never on "vibes."
2. The failure is *new* (not already in the reflection memory for this task).
3. Budget remaining > cost of one reflection + one corrective step.

Do NOT reflect when: the last action succeeded; the same failure repeats twice (that means reflection failed once already — escalate to replan or stop instead); or the model would critique its own output with no external signal (arXiv:2310.01798).

### Test-time compute menu (in order of cost-effectiveness for coding)

1. **Rejection sampling + test-based filter** (cheapest): sample k patches, run test suite, keep passers. No verifier model needed. CodeMonkeys' model-generated tests are the weak link — treat them as ranking signal only, never accept/reject.
2. **Self-consistency**: only where answers are discrete and comparable (choose-a-fix, localize-to-file). Not for diffs — no majority vote over patches; vote over *test outcomes* instead.
3. **Verifier model / LLM-judge**: one final selection trajectory (CodeMonkeys style) beats per-step critics. Cap selection rounds; watch Goodhart (arXiv:2210.10760).
4. **PRMs**: proven in math (78.2% vs 72.4% ORM, arXiv:2305.20050) but require step-level labels; no equivalent public PRM exists for code-edit trajectories. [UNVERIFIED for code domain] — treat as research, not a dependency.
5. **Budget forcing / "Wait"** (s1): harness-applicable without training — if the provider exposes reasoning controls, a "continue and reconsider" nudge before giving up gained +6.7pp on AIME24. Cheap to try; also demonstrably causes loops when over-applied.

## Recommended design for mcode

**Core**: single-threaded ReAct loop (one LLM call → tool calls → observations → repeat) over an append-only event log. **The plan lives in session state** — a structured field on the session, written by the `todo` extension when loaded and readable regardless. Recitation (`21` §Conditional assembly) reads session state, not the extension, so it survives the extension being disabled; with no plan stored it degrades to restating the user's goal (OpenHands pattern, simplified: `EventKind { UserMsg, AssistantMsg, ToolCall, ToolResult, Usage, Note, … }` — the kind set is closed and defined in `20` — serializable, replayable, doubles as the audit trail and crash-resume state). No hidden state outside the log.

**The loop state machine** (`mcode-loop`):

```
States: Idle, Plan, Act, Observe, Verify, Reflect, Replan, Handoff, Done, Failed
Budget B = {steps, tokens_in+out, usd, wall_ms} decremented per event.

Idle    --user_task-->      Plan
Plan    --plan_written-->   Act          [guard: B.steps > reserve]
Plan    --budget_exhausted--> Failed
Act     --tool_calls-->     Observe
Act     --no_tool_calls-->  Verify       [model claims done]
Observe --results_appended--> Act
Observe --thrash_detected--> Reflect   [guard: reflect_count < 2]
Observe --hard_error-->     Reflect
Reflect --diagnosis-->      Act          [same plan, new approach]
Reflect --same_failure_2x-> Replan
Replan  --new_plan-->       Act          [guard: replan_count < 2]
Replan  --no_new_plan-->    Handoff      [report to user with state]
Verify  --tests_pass-->     Done
Verify  --tests_fail-->     Reflect
Verify  --no_tests-->       Handoff      [ask user; never self-certify]
Any     --B exhausted-->    Handoff      [never Failed on budget; emit partial state]
```

Guards and accounting rules:

- **Termination conditions** (checked in order, before each LLM call): (1) `Done`/`Handoff` reached; (2) `B.steps == 0`; (3) `B.usd <= 0` or `B.tokens <= 0`; (4) user interrupt event; (5) provider hard error after the retry policy is exhausted. The budget is also re-checked when tool results land: a tool call that spends the last step must end the run at the Observe state, not after one more model call. — **retry policy is owned by `15` §Provider abstraction** (exponential backoff with full jitter, cap ~30 s, max 5 attempts), not restated here. Terminal states always emit a structured summary: goal, actions taken, last failure, remaining budget.
- **Thrash detection** (the guard that matters most): hash each tool call as `(tool_name, canonicalized_args)` and keep a rolling window. **Two constants only, both defaults to tune against the eval suite**: `repeat_limit = 3` (identical hash seen 3× in the window → Reflect with the repeated calls injected as evidence) and `window = 12` calls. Escalation is monotonic on repeat count — 2× limit → Replan, 3× → Handoff — rather than a ladder of hand-picked numbers. Also escalate on no-progress: workspace content unchanged across `2 × repeat_limit` Act rounds, or the same test-failure signature at `repeat_limit`. The mechanism is the design; the numbers are calibration.
- **Plan and Act share one request when the plan needs no tools.** Every request carries the full prefix (system prompt, instruction chain, tool schemas), so issuing one for Plan and another for Act charged a tool-free turn — a greeting, a question, a one-line edit — for the prefix twice. Plan now owns the turn's first request, and when its response carries no tool call that response *is* the answer: Act reuses it (the `plan_answered_` flag) and the run proceeds to Verify. When the response does carry tool calls, they are dispatched by Act exactly as before, so the observable state sequence is unchanged and **no tool call is ever skipped**. Act dispatches its pending calls *before* the step-budget check, so a request that spends the last step still executes what the model asked for.
- **Budget accounting**: usage is carried by **`Usage` events**, not by every event — the `20` envelope is `{v, seq, ts, kind, run, turn, step, payload}` and only usage-bearing kinds populate cost fields. One `Usage` event per model response carries `{tokens_in, tokens_out, cached_read, cache_write, usd_est, wall_ms}`; the loop folds them into the run budget. `usd_est` comes from the provider pricing table in config. Default caps: 100 steps, 2M tokens, $5/run (all config-overridable; `--max-budget-usd`). Subagents get child budgets carved from the parent; the parent decrements by child spend on child completion.
- **Cost-aware stopping**: when `B.usd < 20%` remaining, inject a system note into the prompt: "budget nearly exhausted — wrap up or report blockers." This converts budget death from silent truncation into a cooperative stop.
- **Futility detection**: a failing run costs ~4× a passing one (token snowball — retries, re-reads, and growing context compound). The thrash guards above are the futility signal; on trigger, **abort with partial state rather than retrying harder**. Budget-aware abort measurably beats unlimited retry, and a resumable Handoff is strictly better than a burned budget.
- **Reflection policy**: `Reflect` state = one LLM call with `{last failure, relevant diff, prior reflections}`; output appended to event log as `Note`. Max 2 per failure class, 4 per run. Never triggered on success. A thrash-triggered reflection has no tool error to name; it is counted under its own failure class rather than left uncounted.

- **Verification gate, v1**: the `Verify` state runs the caller-configured verification command through the `bash` tool as `{"command": …}`. With no command configured the transition is `Verify --no_tests--> Handoff` — never self-certification. `tests_pass` means the result's `exit_code` is zero and the command did not time out (`Done`); `tests_fail` means a non-zero exit, a timeout, or a failed spawn (`Reflect`, subject to the reflection caps). The tool call's own `ok` is NOT the gate: a non-zero exit is a successful tool call whose result carries the exit code, and reading `ok` would stamp every failing run as done. The command is a run-level field, not a config key yet; M3 owns the real gate.
- **Selection phase (optional, `--best-of n`)**: fork the event log at Plan, run n Act/Verify loops, then one judge trajectory picks the winner by test outcomes first, LLM-judge second. This is CodeMonkeys' selection in miniature; keep n ≤ 4 by default.

## Session branching

`--best-of` and `/fork` need the log to branch. The append-only log stays the **source of truth**; a branch is a **view over it**, not a second log. That preserves every invariant (replay, resume, event `seq`) while allowing divergent futures.

**Representation.** Each session has an immutable `session_id` and an active `branch_id`. A branch is created by `mcode.session.fork(at_seq, label) -> branch_id`, which appends a `branch.created` event carrying `{branch_id, parent_branch, at_seq, label}`. The new branch's **lineage is an ordered list of `(branch_id, at_seq)` cut points** back to the root. Nothing is copied: the child reads the parent's events up to `at_seq` through the lineage, then appends its own.

**Reading.** Resolving a branch's event stream walks the lineage and concatenates — parent prefix up to each cut, then the branch's own events. Cached per `(branch_id, up_to_seq)`, so ordinary replay costs nothing extra.

**`seq` stays global and monotonic across branches.** It is a log offset, not a per-branch turn counter. Two branches of one parent share `seq` values over their common prefix and diverge after — which is what makes branch diffing cheap and keeps `20`'s ordering guarantee intact.

| Operation | Semantics |
|---|---|
| `/fork [label]` | Branch from the current turn. The cheapest useful point is just after `Plan` (`--best-of` uses exactly this) |
| `/tree` | Render the branch forest: lineage, labels, per-branch turn count, last-message summary |
| `/switch <branch_id>` | Change the active branch. Append-only; nothing is rewritten |
| `/diff-branch <a> <b>` | First divergent `seq`, then the two tails — the shared prefix is skipped by construction |
| `mcode resume` | Resumes the **active** branch; `--branch <id>` resumes another. Branch state persists with the session, not in a side file |

**Why not copy the log.** Copying makes `seq` ambiguous, doubles storage per fork, and breaks `20`'s "events are immutable and ordered" contract. A lineage list is a few dozen bytes per fork.

**Garbage collection.** A branch with no descendants and no events of its own is dropped at session close; everything else is kept, because a fork is a user decision.

C++23 mapping: state machine as a plain `enum class` + `switch` in one function (no framework, no vtable dispatch); event log as `std::vector<Event>` flushed to JSONL per event (crash-safe); budget as a small struct decremented in one place. The entire loop fits in ~300 lines — mini-SWE-agent's 100-line scale, plus guards.

## Traps

- **Stopping on a thinking loop instead of breaking it.** A model that repeats itself is usually one step from finishing, so aborting the run throws the task away and a restart re-pays for the whole context. The loop is a symptom; a short action-oriented steer breaks it far more cheaply. mcode discards the looping message, appends `"You repeated yourself without making progress. Stop describing the next step and do it now."`, and re-asks — twice at most, then the normal failure path. Detection is exact contiguous repetition (a periodic tail) or a near-duplicate of the previous turn; both are conservative because a false positive discards a good answer.
- **A fuzzy loop signal.** An early-warning threshold — shingle novelty, block repeat, fractional period — false-triggers on legitimate long structured thinking, where exact contiguous repetition never does. pi-repetition-guard shipped a two-stage design, measured every early signal against real content, and collapsed it to the single exact one. Do not add a fuzzy pre-stage back.
- **The guard's known miss, observed live.** In a real session a model emitted a ~300-word essay, a two-sentence aside, then the same essay again. The two copies are not contiguous, so the exact-period signal did not fire and the run was not steered. This is the deliberate tradeoff above, not a defect: catching it needs the near-repetition-dominance signal, which is the one measured to false-trigger on templated thinking. The pair of copies is a *variation* loop; the current guard handles the tape loop and the cross-turn restatement. Widening it means accepting false positives, which discard a good answer, so it is left narrow and recorded here.
- **Reflection loops**: Reflect→Act→same failure→Reflect… Without the "same failure 2× → Replan" edge, reflection becomes the thrash. Cap reflection per failure class, not just per run.
- **Context rot from full history**: SWE-agent's full-history ablation lost 3pp. Summarize or window observations; never let tool output exceed a per-event token cap.
- **Self-generated tests as ground truth**: 16% false-positive rate (Reflexion/MBPP). Generated tests rank candidates; repo tests decide.
- **Goodhart against your own verifier**: best-of-N against an LLM judge over-optimizes the judge, not correctness (arXiv:2210.10760). Cap judge rounds; prefer mechanical signals.
- **Silent budget death**: killing the loop at max_turns without emitting partial state loses all work. Always land in Handoff with a resumable event log.
- **A log that records activity but not conversation.** mcode's first session log held tool names and pass/fail flags, so `--continue` restored a sequence number and the model started from an empty history. A resumable session needs the messages themselves.
- **Plan churn**: re-planning every k steps lets the model undo committed work. Re-plan only on guard triggers, and diff new plan against old — reject plans that revisit completed steps.
- **Over-parallel loops**: ToT-style branching in the main loop costs 5–100× tokens for gains only when intermediate states are verifiable. Keep search in the optional selection phase.
- **Reasoning-model double-think**: DeepSeek-R1-class models already emit self-verification internally; layering a harness-side Reflect state on them doubles cost for ~no gain. Gate Reflect off when the provider reports `reasoning_content`.

## Open questions

- Does a code-domain PRM exist that beats test-based filtering at equal cost? No public evidence found. [UNVERIFIED]
- Optimal thrash window size (12 calls here) is a guess; needs instrumentation on real sessions.
- Should Handoff auto-resume when the user replies, or start a fresh Plan? Affects event-log replay semantics.
- Do reasoning models change the reflection calculus enough that Reflect should be plan-edit-only?

## Sources

- ReAct — https://arxiv.org/abs/2210.03629 (abstract fetched)
- Self-Consistency — https://arxiv.org/abs/2203.11171 (abstract fetched)
- Reflexion — https://arxiv.org/abs/2303.11366 (abstract + HTML full text fetched)
- Self-Refine — https://arxiv.org/abs/2303.17651 (abstract fetched; ablations via search-verified full text)
- LLMs Cannot Self-Correct Reasoning Yet — https://arxiv.org/abs/2310.01798 (abstract fetched)
- Tree of Thoughts — https://arxiv.org/abs/2305.10601 (abstract + HTML full text fetched)
- SWE-agent — https://arxiv.org/abs/2405.15793 (abstract fetched; ablation table via [NeurIPS PDF](https://papers.neurips.cc/paper_files/paper/2024/file/5a7c947568c1b1328ccc5230172e1e7c-Paper-Conference.pdf))
- Agentless — https://arxiv.org/abs/2407.01489 (abstract fetched); Verified numbers via [FSE'25 PDF](https://lingming.cs.illinois.edu/publications/fse2025.pdf) (search-verified)
- OpenHands — https://arxiv.org/abs/2407.16741 (abstract fetched); SDK event architecture via https://arxiv.org/pdf/2511.03690 (search-verified)
- CodeMonkeys — https://arxiv.org/abs/2501.14723 (abstract fetched)
- Large Language Monkeys — https://arxiv.org/abs/2407.21787 (abstract via search)
- DeepSeek-R1 — https://arxiv.org/abs/2501.12948 (abstract fetched)
- s1: Simple Test-Time Scaling — https://arxiv.org/abs/2501.19393 (abstract fetched; AIME24 numbers via [ACL Anthology](https://aclanthology.org/2025.emnlp-main.1025.pdf))
- Let's Verify Step by Step — https://arxiv.org/abs/2305.20050 (abstract fetched; best-of-N detail search-verified)
- Scaling Laws for Reward Model Overoptimization — https://arxiv.org/abs/2210.10760 (abstract fetched)
- ReWOO — https://arxiv.org/abs/2305.18323 (abstract fetched)
- mini-SWE-agent — https://github.com/swe-agent/mini-swe-agent; https://www.swebench.com/ (fetched)
- Claude Code agent loop docs — https://code.claude.com/docs/en/agent-sdk/agent-loop (search-verified)
- pi-repetition-guard — https://pi.dev/packages/@capdiem/pi-repetition-guard (fetched). The two-stage-to-single-signal collapse, the action-oriented steer wording, the retry budget of 2, and the measured root cause (a long-context tracking failure, not a sampling bug) all come from this package's README.
- Infinite Agentic Loops in LLM Agents — https://arxiv.org/html/2607.01641v1 (HTML full text fetched). IALs as feedback paths without an effective bound; the case for a bound that covers the path rather than merely existing near it.
