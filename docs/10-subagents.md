# Subagents

> **Status: shipped in 0.0.3** as `agent/subagent.hxx`. This doc was
> rewritten in the fourth-batch research pass and is now the authority for the
> design; `docs/43-fourth-batch.md` Decision 8 records the decision and the
> sequencing (last, because fan-out multiplies an unbounded-execution problem —
> subagents before the `12` trust gate would run N copies of code the user never
> approved).

> TL;DR: Ship subagents — **read-only only**. They pay for themselves as context firewalls (explore 10K+ tokens, return ≤2K) and as clean-context reviewers, and the evidence for both is convergent across every harness that ships them. Do **not** ship parallel writers in v1: the strongest published multi-agent coding results come from either file-disjoint tasks with a perfect test oracle, or from systems that measurably underperform a plain single agent. One writer, one tree, one integrator.

## State of the field

| System | Architecture | Key evidence |
|---|---|---|
| Cognition "Don't Build Multi-Agents" (Yan, Jun 2025) | Single-threaded linear agent + compaction; subagents only for well-defined read-only questions | Principles: (1) share full traces, not messages; (2) actions carry implicit decisions → parallel writers conflict (Flappy Bird example) |
| Cognition "Multi-Agents: What's Actually Working" (Yan, Apr 2026) | Narrowed position: multi-agent works when **writes stay single-threaded**; agents contribute intelligence, not actions | Devin Review: ~2 bugs/PR, ~58% severe; clean-context reviewer beats shared-context; "smart friend" fails when the primary is weak (SWE-1.5), works cross-frontier |
| Anthropic multi-agent research system (Jun 2025) | Orchestrator-worker; Opus lead + Sonnet subagents, parallel spawn | **+90.2%** over single Opus 4 on internal research eval; multi-agent ≈ **15×** chat tokens (agents ≈ 4×); token usage explains **80%** of BrowseComp variance; 3–5 parallel subagents cut research time up to **90%** |
| [CC] subagents / Task tool | Isolated context per subagent, own system prompt + tool allowlist, returns result only | Built-ins: Explore (read-only), Plan (read-only), general-purpose; `isolation: worktree` for writers; 15k-token cap on agent descriptions; docs warn against same-file parallel edits and subagent-to-subagent chat |
| Anthropic "Building a C compiler" (Carlini, Feb 2026) | 16 parallel agents, bare git repo as hub, file-lock task claiming, no orchestrator | ~2,000 sessions, 2B input / 140M output tokens, ~$20k → 100k-LOC compiler; "merge conflicts are frequent"; agents overwrote each other until tasks were made independent (GCC-oracle split) |
| SWE-agent / mini-SWE-agent | Single agent loop, `HistoryProcessor` compression, no delegation | ACI paper (arXiv 2405.15793) attributes performance to the interface, not delegation; SWE-agent is maintenance-only, successor keeps the single-agent loop |
| OpenHands SDK | Delegation optional via `TaskToolSet` + `register_agent`; V1 keeps one conversation as source of truth | Parallel delegation exists (`tool_concurrency_limit`); docs warn shared-workspace writes cause races |
| Codex CLI `approvals_reviewer` / `/review` | Two distinct reviewers: `/review` reads a diff and **changes nothing**; `auto_review` swaps the human at the sandbox boundary for a reviewer agent | Reviewer sees a compact transcript — **not** the main agent's chain-of-thought; denials return "find a materially safer path or stop"; circuit breaker trips after **3 consecutive or 10 of the last 50** denials; timeouts are surfaced as *not* proof of unsafety |
| opencode agents | `primary` (owns the thread, may call `task`) vs `subagent` (`@general`, `@explore`, `@scout`) | Read-only is a **permission key**, not a prompt: `read`/`edit`/`bash`/`task` = `allow\|ask\|deny`; `permission.task = deny` removes a subagent from the Task description entirely; `max_steps` forces a summarization-only final turn |
| Cline | `spawn_agent` subagents; a heavier `Agent Teams` tier (coordinator + task board + mailbox in `teams.db`) | Tool set follows the *mode*: Act = read/edit/run, Plan = no file-editing tools and destructive shell refused — but "some commands, such as `python -c`, can write files"; subagents can spawn subagents; approving a spawn approves its whole subtree; VS Code/JetBrains extensions do **not** use subagents |

## What works / what doesn't

**Works**
- Read-only subagents as context firewalls: explore tens of thousands of tokens, return 1–2K-token summaries (Anthropic's context-engineering post gives the 1–2K figure). Highest value, lowest risk; both Cognition and Anthropic converge on it.
- Clean-context verification: a reviewer that does **not** inherit the writer's context finds more bugs (Cognition: 2 bugs/PR, 58% severe). Context rot is the mechanism — shorter context, better attention. Codex implements the same separation, and deliberately withholds chain-of-thought.
- Explicit effort scaling: Anthropic embeds rules (simple fact: 1 agent, 3–10 tool calls; comparison: 2–4 subagents, 10–15 calls; complex: 10+ subagents). Early agents without them spawned 50 subagents for a trivial query.
- One-shot task specs: vague delegation ("research the semiconductor shortage") made subagents duplicate work or misread the task. Each needs objective, output format, tool guidance, boundaries.
- Artifact/filesystem handoff: subagents write outputs to storage and pass lightweight references back — Anthropic names this the fix for the "game of telephone."
- Enforced, not prompted, restriction: opencode's permission keys and [CC]'s `tools:` frontmatter both make read-only a property of the tool set. Cline's Plan mode shows what happens when it is only *mostly* structural (`python -c` still writes).

**Doesn't work**
- Parallel writer swarms on shared state: Cognition's core claim, confirmed empirically by Carlini — agents "hit the same bug, fix that bug, and then overwrite each other's changes" until work was partitioned.
- Weak-primary → strong-subagent escalation ("smart friend" inverted): failed for SWE-1.5, because a weak model cannot tell when it is out of its depth or formulate the question.
- Synchronous-only orchestration at scale: Anthropic flags lead-waits-for-all as a bottleneck — no mid-flight steering, one straggler blocks the batch.
- Subagent-to-subagent communication: [CC] does not allow it; Cognition found cross-agent messages "don't happen by default" because models are not trained for it.
- Blanket approval inheritance: Cline's "approving the `spawn_agent` call approves everything the subagent then does" is exactly the property mcode must not copy (`12`).

### Does it help on *coding* tasks, or only research/search?

Every published *win* is research-shaped: breadth-first, read-only, summary-as-result. For code the counter-evidence is stronger than the pro-evidence:

- **MAST** (arXiv 2503.13657) annotated 150+ traces across 5 MAS frameworks (MetaGPT, ChatDev, HyperAgent, AppWorld, AG2): 14 failure modes in 3 categories (specification/design, inter-agent misalignment, task verification), κ = 0.88. Headline: "the gain in accuracy or performance remains minimal compared to single agent frameworks"; ChatDev correctness "can be as low as 25%"; best-effort interventions (better role specs + orchestration) bought **+14%** and did not clear the failures. The paper's conclusion is structural redesign, not better prompts.
- **Agentless** (arXiv 2407.01489) deleted the agent loop: a fixed localize → repair → validate pipeline scored **32.00%** on SWE-bench Lite at **$0.70** — best score and lowest cost among the open-source agents it compared against. Its framing: "do we really have to employ complex autonomous software agents?"
- **SWE-agent** (arXiv 2405.15793) credits the agent–computer interface, not delegation.
- The one large coding win (Carlini's compiler) had a near-perfect test oracle and was made file-disjoint by construction. Cognition's 2026 post says the same of every such demo: they "share a property most real software doesn't: a simple, verifiable success criterion."

### Resolving Cognition vs Anthropic

The positions look opposed but attack different tasks:

- Anthropic's 90.2% is **breadth-first research** — independent directions, read-only, results are summaries. Context sharing is unnecessary because subtasks are independent by construction; the 15× tokens is the price of parallel attention.
- Cognition's prohibition targets **write-heavy coding**, where every edit embeds implicit decisions (style, edge cases, naming) that conflict when made blind.

Anthropic agrees with the restriction: "most coding tasks involve fewer truly parallelizable tasks than research", and domains that "require all agents to share the same context" are "not a good fit". Cognition's 2026 update concedes the read-only and review classes outright. **The disagreement is about write vs read, not about multi-agent per se.** Both endorse read-only fan-out and both reject parallel writes to shared state; mcode encodes exactly that split, which is also `01`'s non-goal ("Not a multi-agent swarm. One writer. Read-only fan-out only").

## Recommended design for mcode

**Decision: ship read-only subagent fan-out in M8; do not ship parallel writers.** Justification is in "Does it help on *coding* tasks" above — the coding wins require a verifiable oracle and file-disjoint tasks, and neither holds for general repo work. `explore` (codebase questions), `plan` (pre-implementation research) and `review` (clean-context diff review) ship; `general` writers stay behind worktree isolation and are off by default.

### Spawn vs inline decision rule

The orchestrator asks: *do I need the trajectory, or only the conclusion?* Only the conclusion → delegate; the trajectory is dead weight in the main context. The numeric thresholds below are the ones already owned by `05`; this doc is where they are enforced.

| Situation | Action | Cost reasoning |
|---|---|---|
| Answerable from ≤3 file reads or one grep | Inline | Spawn overhead (fresh system prompt + re-orientation ≈ 5–15K tokens) exceeds the read cost |
| Exploration needing ≥10 files / ≥30K tokens of tool output | Spawn read-only | Trajectory ≥30K tokens discarded, ≤2K summary returned — the context saving is the point, not the token saving |
| Result needed verbatim (exact code, long logs, test output) | Inline | Summarization destroys the payload; round-trip through a summary loses fidelity |
| N ≥ 2 independent read-only investigations (map call sites, compare designs) | Fan out N read-only subagents | Wall-clock ÷ N; token cost ×N on a cheap model; results disjoint, so no merge risk |
| Sequential/dependent steps | Inline | Each subagent hop adds latency plus fidelity loss; one context is strictly better |
| Fresh-perspective review (diff review, security pass, "refute my claim") | Spawn reviewer with **zero** shared context | Clean context is the feature (Cognition's 58%-severe finding); never share the writer's trace |
| Edit touching a file another in-flight writer touches | Inline (serialize) | Conflict cost exceeds the parallelism gain; Carlini's overwrite failure mode |
| K disjoint file sets, mechanical per-slice change | **Not shipped in v1.** If it is ever enabled: K writers, each in its own `git worktree`, single integrator | Parallelism is real only if isolation is real; Carlini's lock-file scheme still produced frequent conflicts |

### Capability restriction — a tool set, not a prompt

Read-only must be structural; a prompt saying "do not edit" is advisory.

- The subagent type carries an **allow-list of registered tools**, resolved at spawn from the same registry as everything else (`06`). `explore`/`plan`/`review` get `read`, `glob`, `grep`, `list` — and nothing else. `write`, `edit`, `apply_patch`, `bash`, `task` are not merely denied; they are absent from the view the subagent's dispatcher consults, so a call for them fails schema validation instead of reaching an approval prompt. This is opencode's `permission.edit = "deny"` and [CC]'s `tools:` frontmatter, implemented once in the registry.
- **No shell in a read-only subagent.** Cline's Plan mode is the cautionary case: it removes file-editing tools but still runs shell, and `python -c` writes files anyway. A read-only subagent that can shell is not read-only. Anything needing shell is `general` (worktree-isolated) or inline.
- The permission engine runs unchanged inside the subagent (`12`). Outside-workspace reads hit the same `ask` rule; with no human to ask, they **fail closed**. A subagent is not a way around the floor.
- **No approval inheritance.** The spawn is approved once; each tool call inside still passes the permission engine. Cline's blanket-subtree approval is the anti-pattern.
- `task` absent from the registry (or `--no-subagents`) is the kill switch — the same shape as Cline's `spawn_agent` toggle and [CC]'s denial of the Agent tool.
- Depth 1: subagents cannot spawn subagents. Cline allows nesting; nothing in the evidence supports it and it multiplies the handoff loss.

### Context boundary

- The subagent gets its system prompt (from the agent definition), the spawn payload, and the repo. It does **not** get parent history, sibling results, or project memory beyond a compact header. This is `05`'s rule: a fresh window, not a fork.
- The parent writes what matters into `context`. Forcing the orchestrator to state its assumptions in text is the mechanism that kills Cognition's "implicit decisions" failure — an unstated constraint cannot be silently mis-inherited.
- Consequence to state plainly: this makes the subagent *dumber* about the task by design. It is only a good trade when the subagent's work is self-contained. Anthropic's own mitigation is the detailed task description (objective, output format, tool guidance, boundaries) — the spec substitutes for shared context.

### Return contract

```json
{
  "agent": "explore",
  "objective": "one sentence, self-contained",
  "context": "constraints the subagent must honor; paths, symbols",
  "output_schema": "findings|verdict|diff",
  "budget": { "turns": 40, "tokens": 150000 }
}
```

Returned:

```json
{
  "summary": "≤2K tokens, harness-capped, truncation marked explicitly",
  "findings": [{ "claim": "…", "file": "src/x.cxx", "line": 412 }],
  "artifacts": [".mcode/artifacts/<run-id>/report.md"],
  "open_questions": ["what the subagent could not determine"],
  "tokens_used": 41230
}
```

- **Summary plus paths, never raw.** Anything larger than the summary goes to `.mcode/artifacts/<run-id>/` and is returned by path — the same `ArtifactRef` used elsewhere (`03`), and Anthropic's named fix for the "game of telephone". The parent reads artifacts lazily.
- `findings` carry `file:line`. A summary without citations is unverifiable, and verification is the whole point of the reviewer type.
- The loss is real: iterative relay chains distort content ("LLM as a Broken Telephone", ACL 2025) and multi-hop summarization loses numeric precision, attribution and conditional nuance. Hence depth 1, citations required, and no verbatim payloads through a summary.
- `open_questions` is mandatory on cancellation, so a partial result is never mistaken for a complete one.

### Budget interaction

- Subagent usage folds into the **run** budget (`01` owns the numbers; `03` owns the `Budget` struct: turns, tokens, wall-clock, cost, subagent count). One accounting — not a per-subagent island. Cline bills subagent tokens to the session; opencode's `max_steps` is the per-agent analogue.
- Caps: **≤4 concurrent** (config), **≤10 total per run** without user consent, plus the per-subagent turn/token budget. Anthropic's early agents spawned 50 subagents for a simple query; the cap is the fix.
- A breach **cancels and harvests**: `std::stop_token` into the subagent loop, partial structured output returned with `open_questions` populated. Losing a 150K-token exploration to a timeout with nothing returned is the failure to avoid.
- Repeated-denial circuit breaker: Codex interrupts a turn after 3 consecutive or 10-in-50 denials. mcode's analogue is the existing repetition guard (`04`): a subagent that keeps re-attempting a denied call is cancelled, not retried.
- Cost honesty: at Anthropic's measured ~15× chat tokens, fan-out is a premium operation. The trade is only rational when parent context is the scarce resource or the subagent runs on a cheaper model — both configurable, neither automatic.

### Coordination, isolation, merge

- **File-based, not message-passing.** No subagent-to-subagent channel. Shared state is artifacts plus the parent's own context. A subagent that discovers something siblings need records it in `findings`/`open_questions`; the *primary* decides whether to re-dispatch. Matches [CC] (report to parent only) and Cognition (manager synthesizes).
- Fan-out is synchronous-batch: spawn N, await all, merge. Anthropic's current production shape, chosen over async steerability for v1. Async steering is an open question, not a feature.
- Writers, if ever enabled, require `isolation: worktree` — `git worktree add .mcode/wt/<run-id> <base>` (snapshot copy outside a git repo), work there, return a diff plus artifact paths, primary integrates one patch at a time with the build/test gate between each. On conflict the primary redoes the slice inline rather than negotiating with the subagent.

### C++23 implementation notes

- A subagent is the same `AgentSession` as the primary with a different system prompt, tool allow-list and budget — no new execution machinery. The `task` tool constructs a session, runs the loop, validates the result JSON.
- Fan-out: `std::jthread` pool (or asio strands) over the provider client; results merged on the main loop thread. Provider clients per-thread or mutex-guarded.
- Cost: config plus one tool schema plus JSON validation. No queue, no database — artifacts are plain files.

## Traps

- **Vague delegation.** "Look into X" → duplicated work, wrong scope. Require objective, boundaries and output format in every spawn.
- **Read-only that is only mostly read-only.** Cline's Plan mode still runs shell, and `python -c` writes files. No shell tool in a read-only subagent.
- **Sharing the writer's trace with the reviewer.** Destroys the clean-context advantage and biases the reviewer toward the writer's assumptions. Codex withholds chain-of-thought for the same reason.
- **Summarizing what must be verbatim.** Code, error text and test output degrade through a summary; return a path instead.
- **Approval inheritance.** Approving the spawn must not approve the subtree.
- **Letting the model choose the fan-out count.** Hard-cap it and put the scaling rules in the orchestrator prompt.
- **Parallel writes without isolation.** Worktree-less races and lock-file-only schemes both fail. One writer in v1.
- **Assuming multi-agent helps coding because it helps research.** MAST and Agentless say the opposite; the wins live in read-only search and review.
- **Description bloat.** [CC] warns at 15K tokens of agent descriptions; keep descriptions short and put detail in the subagent's own system prompt.
- **Treating the subagent as a cheaper model instead of a smaller context.** The benefit is the isolation, not the price.

## Open questions

- Async steering: should the primary be able to message a running subagent? Anthropic says that is where the field is heading. Defer until sync-batch hurts in practice.
- Resumable subagents: persist subagent state so a killed one resumes instead of restarting ([CC] community reports of resumable subagents — `[UNVERIFIED]`).
- Optimal summary size: 1–2K tokens is Anthropic's reported norm, not a measured optimum for coding. Instrument the correlation between summary length and parent rework.
- Model tiering for subagents: Anthropic runs Sonnet subagents under an Opus lead; whether a cheap model preserves review quality is unmeasured here.
- Whether read-only fan-out actually earns its 15× on *coding* repos, as opposed to research corpora. This is the ablation to run before widening the feature (`11`).

## Sources

- https://cognition.com/blog/dont-build-multi-agents — single-threaded default; "actions carry implicit decisions"
- https://cognition.com/blog/multi-agents-working — writes stay single-threaded; clean-context review; smart friend; manager/child delegation; 2 bugs/PR, 58% severe
- https://www.anthropic.com/engineering/multi-agent-research-system — 90.2%, 15×/4× tokens, 80% of BrowseComp variance, 3–5 parallel subagents, 90% latency cut, effort-scaling rules, artifact spillover
- https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents — compaction / note-taking / sub-agent isolation; 1–2K-token returns; context rot
- https://www.anthropic.com/engineering/building-c-compiler — 16 agents, file locks, "merge conflicts are frequent", independent tasks required
- https://arxiv.org/abs/2503.13657 — MAST: 14 failure modes, 3 categories, κ = 0.88, ChatDev correctness as low as 25%, +14% from interventions
- https://arxiv.org/abs/2407.01489 — Agentless: 32.00% SWE-bench Lite at $0.70 without an agent loop
- https://arxiv.org/abs/2405.15793 — SWE-agent: agent–computer interface, not delegation
- https://swe-agent.com/latest/background/architecture/ — single `Agent.forward()` loop plus `HistoryProcessor`
- https://learn.chatgpt.com/docs/sandboxing/auto-review — reviewer at the approval boundary; 3-consecutive / 10-of-50 circuit breaker; no chain-of-thought to the reviewer
- https://learn.chatgpt.com/docs/code-review — `/review` reads a diff and changes nothing
- https://opencode.ai/docs/agents/ — primary vs subagent; permission keys including `task`; `max_steps`; `@general`/`@explore`/`@scout`
- https://docs.cline.bot/features/subagents — `spawn_agent`, mode-dependent tool sets, `python -c` write caveat, approval covers the subtree
- https://aclanthology.org/2025.acl-long.371.pdf — "LLM as a Broken Telephone": distortion compounds across iterative handoffs
- [CC] docs (sub-agents, best practices, "how and when to use subagents") — fetched; the documentation mirror redacts its own hostname, so no stable URL is quoted here
- https://github.com/OpenHands/docs/blob/main/sdk/arch/design.mdx — delegation composable; one conversation as the source of truth
- https://github.com/OpenHands/software-agent-sdk/blob/main/examples/01_standalone_sdk/25_agent_delegation.py — `TaskToolSet` + `register_agent`
