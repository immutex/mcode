# Subagents & Multi-Agent Design

> TL;DR: mcode ships one primary agent plus read-only subagents for context isolation and parallel research; parallel writers only with git-worktree isolation and a single integrator. Spawn when the subagent's *summary* (≤2k tokens) is worth more than its *trajectory* (≥30k tokens).

## State of the field

| System | Architecture | Key evidence |
|---|---|---|
| Cognition "Don't Build Multi-Agents" (Yan, Jun 2025) | Single-threaded linear agent + compaction; subagents only for well-defined read-only questions | Principles: (1) share full traces, not messages; (2) actions carry implicit decisions → parallel writers conflict (Flappy Bird example) |
| Cognition "Multi-Agents: What's Actually Working" (Yan, Apr 2026) | Narrowed position: multi-agent works when **writes stay single-threaded**; agents contribute intelligence, not actions | Devin Review: ~2 bugs/PR, ~58% severe; clean-context reviewer beats shared-context; "smart friend" fails when primary is weak (SWE-1.5), works cross-frontier |
| Anthropic multi-agent research system (Jun 2025) | Orchestrator-worker; Opus lead + Sonnet subagents, parallel spawn | **+90.2%** over single Opus 4 on internal research eval; multi-agent ≈ **15×** chat tokens (agents ≈ 4×); token usage explains **80%** of BrowseComp variance; 3–5 parallel subagents cut research time up to **90%** |
| Claude Code subagents/Task tool | Isolated context per subagent, own system prompt + tool allowlist, returns result only | Built-ins: Explore (read-only), Plan (read-only), general-purpose; `isolation: worktree` for writers; 15k-token cap on agent descriptions; docs explicitly warn against same-file parallel edits and subagent-to-subagent chat |
| Anthropic "Building a C compiler" (Carlini, Feb 2026) | 16 parallel agents, bare git repo as hub, file-lock task claiming, no orchestrator | ~2,000 sessions, 2B input / 140M output tokens, ~$20k → 100k-LOC compiler; "merge conflicts are frequent"; agents overwrote each other until tasks were made independent (GCC-oracle split) |
| SWE-agent / mini-SWE-agent | Single agent loop, HistoryProcessor compression, no delegation | ACI paper (arXiv 2405.15793) attributes performance to interface design, not multi-agent; SWE-agent now maintenance-only, successor keeps single-agent style |
| OpenHands SDK | Delegation optional via `TaskToolSet` + `register_agent`; V1 favors one conversation as source of truth | Parallel delegation exists (`tool_concurrency_limit`) but docs warn shared-workspace writes cause races |

## What works / what doesn't

**Works**
- Read-only subagents as context firewalls: explore tens of thousands of tokens, return 1–2k token summaries (Anthropic context-engineering post gives the 1–2k figure). This is the highest-value, lowest-risk pattern; both Cognition and Anthropic converge on it.
- Clean-context verification: a reviewer that does NOT inherit the writer's context finds more bugs (Cognition: 2 bugs/PR, 58% severe). Context rot (Chroma research) is the mechanism — shorter context → better attention.
- Explicit effort scaling: Anthropic embeds rules (simple fact: 1 agent, 3–10 tool calls; comparison: 2–4 subagents, 10–15 calls; complex: 10+ subagents). Early agents without this spawned 50 subagents for trivial queries.
- One-shot task specs: Anthropic found vague delegation ("research the semiconductor shortage") made subagents duplicate work or misinterpret; each needs objective, output format, tool guidance, boundaries.
- Artifact/filesystem handoff: Anthropic's appendix recommends subagents write outputs to storage and pass lightweight references back, minimizing the "game of telephone."

**Doesn't work**
- Parallel writer swarms on shared state: Cognition's core claim; confirmed empirically by Carlini (agents "hit the same bug, fix that bug, and then overwrite each other's changes" until work was partitioned).
- Weak-primary → strong-subagent escalation ("smart friend" inverted): failed for SWE-1.5 because the weak model can't tell when it's out of its depth or what to ask.
- Synchronous-only orchestration at scale: Anthropic flags lead-waits-for-all as a bottleneck (can't steer mid-flight, one straggler blocks the batch).
- Subagent-to-subagent communication: Claude Code explicitly doesn't allow it; Cognition found cross-agent messages "don't happen by default" because models aren't trained for it.

## Resolving Cognition vs Anthropic

The positions look opposed but attack different tasks:

- Anthropic's 90.2% win is on **breadth-first research** — independent directions, read-only, results are summaries. Context sharing is unnecessary because subtasks are independent by construction; the "15× tokens" is the *cost of buying parallel attention*.
- Cognition's prohibition targets **write-heavy coding** — where every edit embeds implicit decisions (style, edge cases, naming) that conflict when made blind.

Anthropic agrees: "most coding tasks involve fewer truly parallelizable tasks than research" and "domains that require all agents to share the same context... are not a good fit." Cognition's 2026 update concedes the read-only/review class. **Resolution: the disagreement is about write vs read, not multi-agent per se.** Both endorse: read-only fan-out yes, parallel writes to shared state no. mcode encodes exactly this split.

## Recommended design for mcode

### Spawn vs inline decision rule

The primary agent asks: *would I keep the trajectory or only the conclusion?* If only the conclusion, delegate — the trajectory is dead weight in the main context.

| Situation | Action | Cost reasoning |
|---|---|---|
| Question answerable from ≤3 file reads or one grep | Inline | Spawn overhead (fresh system prompt + re-orientation ≈ 5–15k tokens) exceeds the read cost |
| Exploration needing ≥10 files / ≥30k tokens of tool output | Spawn read-only | Trajectory ≥30k tokens discarded; ≤2k summary returned. Net context saving ≈ 15× at ~1/15 the main-window cost. Same trade Claude Code's Explore makes |
| Result needed verbatim (exact code, long logs) | Inline | Summarization destroys the payload; round-trip through a summary loses fidelity |
| N ≥ 2 independent read-only investigations (map sites, compare designs) | Fan out N read-only subagents in parallel | Wall-clock ÷ N; token cost ×N but on cheap model; results disjoint so no merge risk |
| Sequential/dependent steps | Inline | Chaining subagents through file handoffs adds latency + fidelity loss per hop; single context is strictly better |
| Fresh-perspective review (diff review, security pass, "refute my claim") | Spawn reviewer, zero shared context | Clean context is the *feature* (Cognition's 58%-severe finding); never share the writer's trace with the reviewer |
| Edit touching files also touched by another in-flight writer | Inline (serialize) | Conflict cost > parallelism gain; Carlini's overwrite failure mode |
| K disjoint file sets, mechanical per-slice change | Fan out K writers, each in own `git worktree` | Parallelism is real only if isolation is real; integrator merges sequentially and resolves conflicts itself |

Hard caps: ≤4 concurrent subagents default (config); ≤10 total per task without user consent; subagent turn budget (default 40) and token budget enforced by harness, hard-canceled on breach.

### Subagent I/O contract

One-shot specification — no implicit context sharing. The spawn tool call is the *entire* universe for the subagent:

```json
{
  "agent": "explore",            // explore | plan | review | general
  "objective": "one sentence, self-contained",
  "context": "decisions/constraints the subagent must honor; file paths, symbols",
  "output_schema": "findings|diff|verdict",
  "artifacts": ".mcode/artifacts/<run-id>/",
  "budget": {"turns": 40, "tokens": 150000}
}
```

Rules:
- The subagent gets: its system prompt (from the agent definition), the spawn payload, and the repo. It does NOT get parent history, CLAUDE.md-equivalents beyond a compact project header, or sibling results. If the parent's context matters, the parent must write it into `context` — forcing the parent to state its assumptions is the point (kills Cognition's "implicit decisions" failure).
- Output is structured JSON: `{summary, findings[], artifacts[], open_questions[], tokens_used}`. `summary` ≤ 2k tokens enforced by harness (truncate + note).
- Anything larger than the summary — file dumps, patch candidates, logs — goes to `artifacts/` as files; the subagent returns paths, not content (Anthropic's game-of-telephone fix). Parent reads artifacts lazily, just-in-time.
- Subagents cannot spawn subagents (depth 1). Flat tree, no recursion, no swarms.

### Writer isolation

- Default: only the primary agent writes. Subagent types `explore`/`plan`/`review` have read-only toolsets enforced by the harness (no Edit/Write tools registered), not by prompt.
- `general` writer subagents require `isolation: worktree`: harness runs `git worktree add .mcode/wt/<run-id> <base>` (or snapshot copy if not a git repo), subagent works there, returns diff/artifact paths; primary integrates. Worktree removed after merge or on failure.
- Merge strategy: primary applies patches one at a time into the main tree, running the build/test gate between each; on conflict, primary re-does the slice inline rather than negotiating with the subagent.

### Coordination model

- **File-based, not message-passing.** No subagent-to-subagent channels. Shared state lives in artifacts (plan file, task claims like Carlini's `current_tasks/` locks). A writer subagent that discovers something siblings need records it in its structured output; the *primary* decides whether to re-dispatch. This matches Claude Code (subagents report to parent only) and Cognition (manager synthesizes).
- Fan-out is synchronous-batch (spawn N, await all, merge) — Anthropic's current production shape, chosen over async steerability for v1 simplicity. Async steering is an open question, not a v1 feature.

### C++23 implementation notes

- Subagent = same `AgentSession` class as primary, different system prompt + toolset bitmask + budget; no new execution machinery. The Task tool is ~a function that constructs a session, runs the loop, validates output JSON.
- Fan-out: `std::jthread` pool or asio strands over the provider HTTP client; results land in a `std::pmr`-backed queue merged on the main loop thread. Provider clients must be per-thread or mutex-guarded.
- Cancellation: `std::stop_token` threaded into every subagent loop; budget breach → request stop → harvest partial structured output (`open_questions` marks incompleteness) rather than discarding.
- Cost: subagent machinery is config + one tool schema + JSON validation; binary growth negligible. No queue/database — artifacts are plain files.

## Traps

- **Vague delegation.** "Look into X" → duplicated work, wrong scope (Anthropic's semiconductor example). Require objective + boundaries + output format in every spawn.
- **Letting the model pick fan-out count unprompted.** Anthropic's early agents spawned 50 subagents. mcode hard-caps and puts scaling rules in the orchestrator prompt.
- **Sharing the writer's trace with the reviewer.** Destroys the clean-context advantage; also biases the reviewer toward the writer's assumptions.
- **Summarizing what must be verbatim.** Code, error messages, and test output degrade through summary; route those inline or via artifacts.
- **Parallel writes without isolation.** Both worktree-less shared-dir races (OpenHands warning) and lock-file-only schemes (Carlini's "merge conflicts are frequent") show this. Worktree or nothing.
- **Description bloat.** Claude Code warns at 15k tokens of agent descriptions; keep per-agent descriptions short, detail in the agent's system prompt (loaded only on spawn).
- **15× token burn by default.** Multi-agent is a premium tool; default inline, make spawning deliberate.

## Open questions

- Async steering: should the primary be able to message a running subagent (Anthropic says this is where the field is heading)? Defer until sync-batch hurts in practice.
- Resumable subagents: persist subagent context to disk so a killed/timeout subagent resumes instead of restarting (Claude Code shipped this in v2.0.60 per community reports — [UNVERIFIED]). Worth prototyping only if long-budget subagents become common.
- Optimal summary size: 1–2k tokens is Anthropic's reported norm, not a measured optimum for coding tasks. Instrument mcode: correlation between summary length and parent rework rate.
- Small-model subagents: Anthropic runs Sonnet subagents under an Opus lead; mcode should support per-agent model config from day one, but whether cheap-model Explore preserves quality on large repos is unmeasured here.

## Sources

- https://cognition.com/blog/dont-build-multi-agents (fetched)
- https://cognition.com/blog/multi-agents-working (fetched)
- https://www.anthropic.com/engineering/multi-agent-research-system (fetched)
- https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents (fetched)
- https://www.anthropic.com/engineering/building-c-compiler (fetched)
- https://code.claude.com/docs/en/sub-agents (fetched)
- https://code.claude.com/docs/en/best-practices (fetched)
- https://claude.com/blog/subagents-in-claude-code (fetched)
- https://swe-agent.com/latest/background/architecture/ (fetched)
- https://arxiv.org/abs/2405.15793 (SWE-agent ACI paper, via search)
- https://github.com/OpenHands/software-agent-sdk/blob/main/examples/01_standalone_sdk/25_agent_delegation.py (via search)
- https://github.com/OpenHands/docs/blob/main/sdk/arch/design.mdx (via search)
