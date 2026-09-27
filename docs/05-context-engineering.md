# Context Engineering

> TL;DR: Context is a finite, degrading resource — budget it like memory in a real-time system: clear tool results early, compact at ~80% fill, externalize state to files, isolate exploration in subagents, and build the prompt append-only for KV-cache hits.

## State of the field

| System / source | What it does | Key evidence |
|---|---|---|
| Chroma "Context Rot" (Jul 2025) | Controlled NIAH/LongMemEval/repeat-task study, 18 LLMs, input length isolated from task difficulty | Performance degrades with length even on trivial tasks; degradation is non-uniform; single distractor measurably hurts, 4 distractors compound it; on LongMemEval all models score significantly higher on ~300-token focused prompts than the full ~113K-token history |
| RULER (COLM 2024, arXiv 2404.06654) | 13 synthetic tasks (multi-needle, tracing, aggregation, QA) at 4K–128K | Only ~half of 17 tested models stayed above the quality threshold at 32K despite all claiming ≥32K; 1→8 needles costs ~15 points; distractors cost up to ~40 points (Yi-34B, 256K config) |
| NoLiMa (arXiv 2502.05167) | NIAH with non-lexical needle–question pairs | At 32K, 11 of 12 models drop below 50% of their short baseline; GPT-4o falls 99.3% → 69.7% |
| Anthropic "Effective context engineering" (Sep 2025) | Names the discipline: smallest set of high-signal tokens; attention budget depletes with n² pairwise attention | Prescribes compaction, structured note-taking, sub-agents; Claude Code keeps summary + 5 most recent files after compact; subagents return 1–2K-token summaries from 10K+-token explorations |
| Manus engineering blog (Jul 2025) | Production agent at ~50 tool calls/task, input:output ratio ~100:1 | KV-cache hit rate is "the single most important metric"; cached Claude Sonnet input was 0.30 vs 3.00 USD/MTok (10×); file system as restorable external memory; todo.md "recitation" against lost-in-the-middle |
| Claude Code docs (2026) | Auto-compact at ~95% of context window (configurable down via `CLAUDE_AUTOCOMPACT_PCT_OVERRIDE`, cannot raise); Sonnet 5 1M window compacts ~967K | Post-compact: re-inject system prompt/CLAUDE.md/plan, re-read ≤5 most recently modified files, files >5K tokens come back as path references; skill bodies capped 5K/skill, 25K total |
| Anthropic Context Editing API | Server-side `clear_tool_uses_20250919`: trigger default 100K input tokens, keep last 3 tool uses; `clear_at_least` to amortize cache invalidation | Clearing invalidates the cached prefix — clear enough per pass to make the re-write worthwhile |
| OpenHands SDK condenser | `LLMSummarizingCondenser`: triggers at `max_size` (default 120 events), keeps first 4 events verbatim, LLM-summarizes the middle; `Condensation` event records `forgotten_event_ids`; cheaper LLM does summarization; manual trigger on context-overflow error | PipelineCondenser composes stages (truncate → summarize) |
| LangChain "Context Engineering" (Jul 2025) | Taxonomy: write / select / compress / isolate context | Cites Cognition: summarization at agent–agent handoffs; Cognition uses a fine-tuned summarizer model |
| Anthropic multi-agent research system (Jun 2025) | Orchestrator + parallel subagents | +90.2% over single-agent Opus 4 on internal research eval; token usage alone explains 80% of BrowseComp variance; agents use ~4× chat tokens, multi-agent ~15× |

**Retrieval vs agentic search.** Anthropic's position (verified in their engineering post): for code, "just-in-time" agentic navigation — grep/glob/file reads over lightweight identifiers — beats pre-computed embedding retrieval, because code is full of exact identifiers, and folder/naming metadata is signal embeddings discard; Claude Code is explicitly hybrid (CLAUDE.md up front, grep/glob at runtime). Third-party evidence is split: one 2026 repo-QA study reports vector retrieval beating "deep agentic search" 65.2% vs 46.2% at less than half the cost per correct answer (arXiv 2608.01507, numbers from search results — [UNVERIFIED] — not read directly), while another LongMemEval-subset study found grep ahead but harness-dependent (arXiv 2605.15184, same caveat). Verdict: agentic grep/glob is the right default for a coding agent; embeddings are an optional accelerator, not a dependency. Avoiding a vector index also eliminates chunking, sync, and stale-index failure modes — a large win for a lightweight binary.

**Where sources disagree.** Claude Code compacts at ~95% of window; that leaves almost no headroom for the summary call itself plus the next turn's tool outputs. OpenHands triggers on event count (120) rather than tokens. Manus avoids summarization entirely when possible, preferring restorable clearing. mcode should pick a token-based trigger below 95% (see table) and treat summarization as the last resort, not the first lever.

## What works / what doesn't

| Technique | Verdict | Numbers |
|---|---|---|
| Tool-result clearing (keep call record, drop payload) | Works; safest, cheapest lever | Anthropic default: trigger 100K input tokens, keep 3 most recent; "safest lightest-touch form of compaction" |
| Whole-conversation compaction | Works, but lossy and destabilizing if aggressive | Claude Code keeps summary + ≤5 recent files, >5K-token files as references; over-aggressive summaries lose "subtle but critical context" (Anthropic) |
| External memory files (NOTES.md / todo.md) | Works; survives compaction and sessions | Manus: recitation counters lost-in-the-middle over ~50-call tasks; Anthropic memory tool ops: view/create/str_replace/insert/delete/rename |
| Subagent isolation | Works for read-heavy exploration | 10Ks of tokens explored → 1–2K returned; multi-agent +90.2% on research evals but ~15× chat token cost — wrong tool for sequential coding work |
| Embedding retrieval over code | Situational | Wins on vocabulary-mismatch queries; loses on exact identifiers; competing 2026 study results (above) |
| Repo map injected into the prompt (Aider-style) | **Doesn't pay** | Most expensive retrieval strategy tested, no positive isolated delta; superseded by query-time structural retrieval (+7.9pp causal) and by grep/read. The cost is paid every turn for a map the model mostly ignores (`11`) |
| Dynamic tool add/remove mid-session | Doesn't work | Manus: invalidates KV cache (tool defs sit at prefix) and confuses the model into hallucinated actions; mask logits instead |
| Erasing failed actions from history | Doesn't work | Manus: "erasing failure removes evidence"; keep errors in context so the model updates its prior |
| Static few-shot repetition in long traces | Backfires | Manus: model mimics context patterns → drift on repetitive tasks; inject structured variation |
| Trusting advertised context windows | Doesn't work | RULER: half of models fail at 32K; NoLiMa: 11/12 below 50% baseline at 32K |

## Recommended design for mcode

### Architecture: four layers, in priority order

1. **Prevention** — never let bulk into the window (progressive disclosure, capped reads, budgeted tool results).
2. **Clearing** — drop re-fetchable tool payloads in place, keep call records (restorable, Manus-style: URL/path always retained).
3. **Isolation** — subagents for exploration that would flood the main window.
4. **Compaction** — summarize only when 1–3 are exhausted.

### Numeric guidance table (defaults; all user-tunable via config)

| Knob | Default | Rationale / source |
|---|---|---|
| Compaction trigger | **80% of usable window** | Claude Code uses ~95%; too tight — leave room for summary call + next turn's tool results. Common community override is 75–80% |
| Usable window | model window − reserved output (e.g. −16K) − 5% safety margin | Compaction itself needs headroom |
| Keep verbatim on compact | first 2–4 events + last **3 turns or 20K tokens** (whichever smaller) | OpenHands `keep_first=4`; recent turns carry working state |
| Keep verbatim, always | user's original task text, current plan/todo, list of modified files | Anthropic compaction guidance: decisions, unresolved bugs, implementation details |
| Post-compact file re-read | ≤5 most recently modified, each capped 5K tokens; larger → path reference | Mirrors Claude Code exactly |
| Compaction summarizer | cheaper/faster model, separate system prompt, maximize recall first then tune precision | Anthropic tuning advice; OpenHands uses cheaper LLM |
| Tool-result clear trigger | **60% of usable window** input tokens | Anthropic API default is 100K on 200K (50%); 60% keeps clearing strictly before compaction |
| Tool uses kept on clear | **3 most recent** | Anthropic default |
| `clear_at_least` | 20% of usable window per pass | Clearing invalidates the cache prefix; small clears don't pay for the re-write (Anthropic `clear_at_least`) |
| Per-tool-result hard cap | **2K tokens inline** (~8K chars); overflow → spill to file, return preview + path | Claude Code hooks: >10K chars → file + preview |
| Search/grep result cap | 50 matches or 2K tokens; paginate | Keeps agent iterating rather than dumping |
| File read default | offset/limit, **100 lines**, numbered; auto whole-file only if <200 lines | SWE-agent's viewport ablation: a 100-line window beats both 30-line and full-file (`11`, arXiv 2405.15793). Numbered lines are needed for edit anchoring |
| Single read max | 1,000 lines / ~10K tokens, then force re-issue with offset | Prevents one `read` from eating 5% of window |
| Re-read policy | never re-send identical file content; serve from session cache with a "unchanged since read N" stub | Cache + dedupe |
| Subagent spawn threshold | expected **>10 tool calls or >30K tokens of reads**, or read-only research phase | Below that, inline; above, isolate. Anthropic: subagents explore 10Ks of tokens, return 1–2K |
| Subagent return budget | **≤2K tokens**, must cite file:line refs; large artifacts written to files, path passed back | Anthropic numbers; "game of telephone" avoidance — artifacts to filesystem |
| Recitation interval | rewrite goal/todo into context every **10 turns** or after each completed phase | Manus recitation |
| Error retention | failed tool calls kept until task phase completes; only then clearable | Manus "keep the wrong stuff in" |
| Context budget warning | 50% (soft warn in status line), 70% (recommend /compact), 80% (auto-compact) | Progressive user signal |
| Session start budget | **system prompt ≤1.5K + tools ≤3.5K + instruction chain ≤2K + skill index ≤1.5K = ≤8.5K tokens** | [CC] representative: system ~4.2K, project CLAUDE.md ~1.8K. This row is the authority. The tool figure covers the **default effective set** (core ≈2–2.5K + tier-1 bundled extensions ≈1–1.5K — `06`, `23`), not core alone; `08`'s byte caps are overflow safety limits, not targets |

### What breaks at which fill level

Two separate things, deliberately not blended — the published studies measured *input length*, not our triggers.

**Evidence (measured by the cited study):**

| Fill (of usable window) | Observed failure mode | Source |
|---|---|---|
| 0–25% | Nominal. Baseline quality | — |
| 25–50% | Distractor sensitivity begins; semantically-similar-but-wrong content pulls answers off-target; degradation is non-uniform across models | Chroma: distractors hurt at all lengths, worse as length grows |
| ~32K absolute | Sharp drop on non-lexical retrieval: 11/12 models <50% of short baseline; GPT-4o 99.3→69.7 | NoLiMa |
| ~50% | Retrieval-across-history tasks measurably degrade; focused ~300-token prompts beat full ~113K histories on LongMemEval for every tested model | Chroma LongMemEval |
| ~70% | Multi-step plans drift; goal recitation needed | Manus (production observation, `[VENDOR]`) |
| 100% | Hard `prompt is too long` error | OpenHands |

**mcode triggers (chosen, not measured — tune against the eval suite):**

| Trigger | Value | Rationale |
|---|---|---|
| Tool-result clearing | 60% | Strictly before compaction; Anthropic's API default is ~50% |
| Compaction | 80% | Below [CC]'s ~95%, leaving headroom for the summary call plus the next turn's tool results |
| Soft warning / recommend compact | 50% / 70% | Progressive user signal in the status line |

No published study measured compaction or clearing thresholds; these are engineering choices with a stated rationale, and `11`'s eval suite is what would move them.

### KV-cache-friendly prompt construction

Manus: cache hit rate is the metric that matters; ~100:1 input:output makes prefill dominate cost. Rules for mcode's request builder:

- **Frozen prefix order**: `tools → system → messages` (Anthropic cache order). Never inject anything mutable before or into system/tools.
- **No timestamps, no cwd-of-the-moment, no git status in the prefix.** Put volatile environment blocks at the *tail* of the last user message, or accept the cache break consciously.
- **Append-only history.** Never mutate a prior message. Clearing/compaction are the only sanctioned rewrites, and each one costs a full cache re-write — hence `clear_at_least` ≥20%.
- **Deterministic serialization.** C++23: emit JSON with fixed key order (write a serializer with explicit ordering, never rely on map iteration); stable float formatting; stable tool-schema ordering (sort by name once at session start).
- **Explicit breakpoints**: one at end-of-tools+system, one at the newest message boundary (Anthropic automatic caching does the latter by moving the breakpoint forward; mcode should replicate that for providers with manual breakpoints).
- **TTL awareness**: Anthropic default cache TTL is 5 min, refreshed on read; a 1-hour TTL costs 2× write but suits interactive sessions with user think-time. Track last-request time; if >5 min elapsed and the next turn is small, consider a cache-keepalive no-op only when the re-write cost exceeds it.
- Cache economics: reads ~0.1× base input price, writes 1.25× (5-min). A 150K-token prefix re-read at 0.1× vs re-prefill at 1.0× is a 10× swing per turn.

### File reads: offset/limit vs whole-file

Default `read(path, offset, limit=100)` with 1-based line numbers. The 100-line default is measured, not arbitrary: SWE-agent's viewport ablation found it optimal against both 30-line windows and full-file reads, and a tighter window keeps relevance-per-token high (Chroma: extra tokens are negative value). Whole-file reads are allowed only when (a) the file is <200 lines, or (b) the model states it will edit broadly and the file is <1,000 lines. Every read records `{path, offset, limit, content-hash}` so clearing can restore a restorable stub (“read lines 1–100 of 1,200, hash abc123”). After compaction, only the ≤5 most recently *modified* files are re-read; everything else becomes a path reference.

### Progressive disclosure

- **Tools**: name + one-line description always in context; full JSON schema loaded on first use (Claude Code's MCP tool search defers schemas; `ENABLE_TOOL_SEARCH=auto` loads upfront only if they fit in 10% of window). mcode: same rule — defer MCP tool schemas, load on demand, keep a per-session loaded-set.
- **Skills**: description line in system tail; body injected only on invocation.
- **Tool results**: preview ≤2K tokens + spill path; the agent decides whether to pull more.
- **Directories**: `glob`/`ls` output capped at 12 entries per listing with a "… N more" marker (this harness's own convention works well).

### Sub-context isolation

Subagent = fresh window with its own system prompt + task text; it does **not** inherit parent history (Claude Code: subagent loads its own CLAUDE.md copy, gets most tools, no recursion). Parent pays only the prompt + result. Spawn when the work is read-heavy and self-contained; never for sequential stateful editing (Anthropic: coding tasks parallelize poorly; Cognition's "don't build multi-agents" argument applies to shared-context work). Subagent results: ≤2K-token summary with file:line citations; bulky artifacts go to files under `.mcode/artifacts/`, referenced by path.

### Token accounting

- Capture `input_tokens / cache_read_tokens / cache_write_tokens / output_tokens` from every provider response; persist per session.
- Maintain a running window-fill estimate: last known input tokens + estimated next-turn delta (tool result sizes are known pre-send — count them before the call).
- Expose a `/context`-style breakdown: system, tools, history, tool results, memory files, free.
- Enforce budgets pre-send: if projected input > usable window, run clearing pass first, then compaction — never send a request you know will 400.
- Expect ~100:1 input:output (Manus); if output ratio is much higher, the agent is narrating instead of acting — prompt problem, not context problem.

## Traps

- **Timestamps in the system prompt** kill the cache from token 0 (Manus's named anti-pattern).
- **Non-deterministic JSON key order** silently invalidates the cache with no error — test serialization stability.
- **Compacting away errors**: the model repeats failed actions without the evidence (Manus).
- **Clearing tool *inputs* by default**: keep call records; clear results only. `clear_tool_inputs: true` is for extreme cases.
- **Compacting at 95%**: the summary call itself plus next tool results can overflow before the compacted context lands.
- **Summarizing with the same model and no dedicated prompt**: generic summaries lose file paths and exact identifiers — the tokens a coding agent needs most. Compaction prompt must demand: paths, symbols, error messages, unresolved questions, next steps.
- **Subagents for everything**: ~15× token cost; coordination failures (duplicate searches, handoff losses) are documented. Isolate reads, not edits.
- **Vector index as a dependency**: stale-index bugs, chunking heuristics, embedding cost — and evidence for code QA is mixed at best. Ship grep/glob; add embeddings later only if vocabulary-mismatch failures show up in evals.
- **Counting events instead of tokens** (OpenHands-style `max_size=120`): 120 events can be 5K or 500K tokens. Budget in tokens.
- **Truncating the middle of a file read silently**: always tell the model what was cut (`… N more lines`) so it can re-read deliberately.

## Open questions

- Optimal compaction trigger likely varies by model family (Chroma shows non-uniform degradation curves). Should mcode ship per-model fill tables, or one conservative 80% default until measured?
- Do 1M-window models actually tolerate deeper fills before degrading, or does context rot scale proportionally? Chroma tested to model maxima but per-model curves differ; no cross-window normalization exists yet.
- Should clearing be client-side (mcode owns history, simpler, cache-aware) or delegated to server-side context-management APIs where available? Client-side is provider-portable; server-side saves tokens in flight.
- Is logit masking (Manus) worth the provider-specific plumbing for tool selection, or is prompt-level "prefer X tool" guidance sufficient at mcode's scale?
- Memory-file GC: notes accumulate across sessions; no studied policy for pruning stale ones without losing load-bearing facts.

## Sources

- https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents — compaction, note-taking, sub-agents, just-in-time retrieval, attention budget
- https://manus.im/blog/Context-Engineering-for-AI-Agents-Lessons-from-Building-Manus — KV-cache rules, 100:1 ratio, 10× cache pricing, recitation, mask-don't-remove, restorable compression
- https://research.trychroma.com/context-rot — 18-model degradation study, distractor and LongMemEval results
- https://arxiv.org/abs/2404.06654 — RULER: effective vs claimed context length
- https://arxiv.org/abs/2502.05167 — NoLiMa: 32K non-lexical collapse
- https://code.claude.com/docs/en/context-window.md — compaction survival table, 5-file re-read, skill caps
- https://code.claude.com/docs/en/env-vars.md — `CLAUDE_AUTOCOMPACT_PCT_OVERRIDE`, `CLAUDE_CODE_AUTO_COMPACT_WINDOW`, `DISABLE_AUTO_COMPACT`
- https://code.claude.com/docs/en/model-config.md — default auto-compact thresholds, Sonnet 5 ~967K
- https://docs.openhands.dev/sdk/arch/condenser — condenser thresholds, keep_first, manual trigger
- https://www.anthropic.com/engineering/multi-agent-research-system — 90.2%, 80% variance, 4×/15× token multipliers, effort-scaling rules
- https://www.langchain.com/blog/context-engineering-for-agents — write/select/compress/isolate taxonomy
- https://platform.claude.com/docs/en/build-with-claude/prompt-caching — cache order, TTLs, pricing multipliers
- https://platform.claude.com/docs/en/build-with-claude/context-editing — clear_tool_uses defaults, cache-invalidation interaction
- https://platform.claude.com/cookbook/tool-use-context-engineering-context-engineering-tools — compaction vs clearing vs memory comparison, API knobs
- arXiv 2608.01507 and 2605.15184 (grep-vs-embeddings studies) — surfaced via search with quoted numbers; not read directly, treat as [UNVERIFIED]
