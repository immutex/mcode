# Memory Architecture for mcode

> TL;DR: Ship memory as plain markdown files (user-authored `AGENTS.md` + agent-authored index + topic files) with an optional SQLite FTS5 index for BM25 retrieval; gate all automatic writes behind user-visible approval; never embed vectors or graphs by default.

## State of the field

### Taxonomy (cognitive-science framing, used by MemoryAgentBench [arxiv.org/abs/2507.05257])

| Type | Content | Lifetime | Coding-agent example |
|---|---|---|---|
| Working context | Active prompt window | Session | Current task, tool results |
| Session scratchpad | Notes within one session | Session | Plan file, todo list |
| Project memory | Repo facts, conventions | Repo lifetime | `AGENTS.md`/`CLAUDE.md` |
| Episodic/trajectory | What happened in past sessions | Weeks–months | Prior failure notes, "tried X, failed because Y" |
| Semantic (long-term) | Distilled facts about user/project | Months+ | "User prefers pnpm", "CI needs Redis" |
| Procedural | Reusable workflows | Months+ | Skills (`SKILL.md` files) |

MemoryAgentBench identifies four competencies a memory system needs: accurate retrieval, test-time learning, long-range understanding, and **selective forgetting** — current systems fail on all four (ICLR 2026 version of the paper).

### Named systems, with evidence

| System | Mechanism | Key measured result |
|---|---|---|
| MemGPT/Letta [arxiv.org/abs/2310.08560] | OS-style paging; LLM self-edits working context via function calls; core memory blocks always in context + searchable archival | DMR: 84.0% vs 79.8% (GPT-4 recursive summaries). Doc-QA: scalability win, not accuracy win — GPT-4 fixed-context beat it in some conditions |
| mem0 [arxiv.org/abs/2504.19413] | Pipeline: extract facts → LLM decide ADD/UPDATE/DELETE → vector store (+optional graph) | LoCoMo: 66.88% vs **72.90% full-context baseline** — memory *lost* on accuracy; won on cost: ~1,764 vs ~26,031 tokens/query, 1.44s vs 17.12s p95. mem0's newer "92.5%" claims are vendor marketing on a different pipeline |
| Zep/Graphiti [arxiv.org/abs/2501.13956] | Bi-temporal knowledge graph (episode → entity → community layers), invalidation of stale edges | LongMemEval-s: 60.2%→71.2% (GPT-4o), median 28.9s→2.58s, 115k→1.6k tokens. Regressed single-session-assistant recall 94.6%→80.4% — extraction loses detail |
| A-MEM [arxiv.org/abs/2502.12110, NeurIPS 2025] | Zettelkasten notes: atomic memory + LLM-generated links + "memory evolution" rewrites old notes | LoCoMo multi-hop F1 27.02 vs MemGPT 26.65 (GPT-4o-mini); ablation: links give most of the gain. Cost: 1,200–2,520 tokens/op, ~5.4s/op (GPT-4o-mini) |
| Generative Agents [arxiv.org/abs/2304.03442] | Memory stream scored by recency (0.995^Δt) + importance (LLM-rated 1–10) + relevance (cosine); reflection when cumulative importance >150 | Removing reflection degraded synthesis tasks; retrieval failures were the dominant error source |
| HippoRAG [arxiv.org/abs/2405.14831] | KG + Personalized PageRank for single-step multi-hop retrieval | Up to +20% over SOTA RAG on multi-hop QA; 10–30x cheaper, 6–13x faster than iterative retrieval (IRCoT) |
| Claude Code auto memory [code.claude.com/docs/en/memory] | Per-project dir: `MEMORY.md` index (first 200 lines/25KB loaded each session) + topic files read on demand; plain markdown, user-editable | Production precedent for "memory as files the agent edits"; index-over-limit returns an error forcing a rewrite |
| Cursor Memories [cursor.com/docs] | Background process proposes memories; **user approval required before save**; scoped per project | Production precedent for approval-gated auto-memory |
| Plain file + grep / SQLite FTS5 | No embeddings; BM25 lexical search (e.g., Engram: single Go binary, SQLite FTS5 [github.com/neoneye/agent-memory-atlas]) | FTS5 is in-process, zero-install; BM25 ranking + column weights built in [sqlite.org/fts5.html] |

### Benchmarks

| Benchmark | Shape | Notable numbers |
|---|---|---|
| LongMemEval [arxiv.org/abs/2410.10813] | 500 questions over multi-session histories; 5 abilities incl. temporal reasoning, knowledge updates, abstention | Commercial assistants + long-context LLMs drop ~30% accuracy vs short-context; proposed fixes: session decomposition, fact-augmented keys, time-aware query expansion |
| LoCoMo [arxiv.org/abs/2402.17753] | ~300-turn, ~9,209-token, ~19-session conversations; QA + event summarization | Widely gamed: an audit claims a large share of answer keys are wrong (community reports, [UNVERIFIED]); treat vendor LoCoMo SOTA claims skeptically |
| MemoryAgentBench [arxiv.org/abs/2507.05257] | Incremental multi-turn; 4 competencies | Finding: no evaluated system (context-based, RAG, external memory modules) masters all four |
| SWE-bench memory ablations | see below | see below |

## What works / what doesn't

**Works:**
- **Small always-loaded index + on-demand detail files** (Claude Code model). Bounded token cost, human-inspectable.
- **Retrieval to dodge huge prompts.** Zep: 98.6% prompt reduction at +11pp accuracy. But note the confound — most of the latency win is just not shipping 115k tokens.
- **Episodic failure memory for repeated mistakes.** Pre-registered SWE-bench Verified study: 33/49 (67.3%) → 38/49 (77.6%) with persistent failure memory; mean per-instance effect +0.24, 95% CI [0, 0.47] [github.com/SaravananJaichandar/coding-agent-memory-benchmark]. Task selection favors memory (built around repeated mistakes) — don't over-extrapolate.
- **Repo instruction files improve efficiency, not correctness.** AGENTS.md/CLAUDE.md ablation over 288 runs: Claude Code 53.3%→55.6%, Codex 58.8%→56.9% pass rates — no detectable correctness gain; ~24% wall-clock reduction in the selective condition on one repo [arxiv.org/abs/2607.27250]. Separate 124-PR study: 28.64% lower median runtime, 16.58% fewer output tokens [arxiv.org/abs/2601.20404].
- **Lexical search at personal scale.** FTS5 BM25 at ~10³–10⁵ memories is fast, deterministic, explainable, zero-dependency.

**Doesn't work / costs more than it returns:**
- **Auto-extracting memories from every turn.** mem0-style pipelines pay 1.2k–2.5k tokens + an LLM call per write (A-MEM's measured cost; mem0 similar), and mem0 still *underperformed full-context* on LoCoMo accuracy. The write-side cost is real; the benefit is not demonstrated for coding agents.
- **Graphs for single-user memory.** Graphiti's gains come from temporal invalidation + compact retrieval, not graph structure per se; extraction actively *lost* detail (94.6%→80.4%). An entity graph over one developer's project notes is over-engineering.
- **Vectors by default.** Embeddings need a model, a store, and re-embedding on edit. Code memories are keyword-dense (symbol names, paths, flags) — lexical search matches them well.
- **Unbounded memory growth.** Stale memories poison future sessions (below). Forgetting is a first-class feature (MemoryAgentBench competency 4), not an afterthought.

## Recommended design for mcode

### Layers

```
<user-config>/mcode/
  AGENTS.md-equivalents: read from repo (user-authored, versioned, NOT mcode's to write)
  projects/<repo-id>/memory/
    MEMORY.md            # index; <=200 lines; loaded every session
    topic-*.md           # one file per topic; read on demand
  projects/<repo-id>/sessions/<id>.jsonl   # episodic trajectory log (append-only)
```

- **Project memory**: mcode reads `AGENTS.md` (nearest file wins, nested supported — the de-facto standard, 60k+ repos [agents.md]). mcode never auto-edits it; it may *propose* an append.
- **Semantic/episodic memory**: `MEMORY.md` index (one line per entry: `- <claim> [topic-file#anchor]`) + topic files with YAML frontmatter. This is the whole persistent store.
- **Scratchpad**: per-session file, deleted on session end unless promoted.
- **Procedural**: skills directory (separate doc); memory only stores *pointers* to skills.

### Storage format

Markdown is the source of truth; SQLite FTS5 is a derived, rebuildable index (`memory.db`, WAL). If the index is deleted, `mcode memory reindex` rebuilds it by scanning the markdown. No server, no embeddings, no graph.

```markdown
---
topic: build
source: user          # user | correction | session-summary  (never: tool-output)
created: 2026-09-27
last_hit: 2026-10-01
hits: 4
supersedes: memory/build.md#old-test-cmd
---
Release builds need `cmake --preset release` first; bare `make` silently
produces a debug binary (observed 2026-09-27, confirmed by user).
```

Schema (conceptual): `memories(id, topic, body, source, created, last_hit, hits, superseded_by)` + FTS5 table over `(topic, body)` with BM25 column weights (topic=3, body=1), kept in sync by triggers.

### Write triggers (the core policy)

1. **Explicit user request** ("remember that…") → write immediately, echo the diff in the TUI.
2. **User correction of the agent** (user overrides agent's claim/behavior) → stage a candidate memory; surface a one-line proposal at session end or next natural pause; write on approval (Cursor's model, not Claude Code's silent write).
3. **Session-summary compaction** → at session end, one bounded LLM pass proposes ≤3 candidates from the session; approval-gated; provenance `source: session-summary`.
4. **NEVER** write from tool output, web content, MCP server responses, or repo file contents. Those are untrusted write channels — the primary memory-poisoning vector (below). If a fact must come from an external source, store it with `source: tool-output` and a lower trust tier that retrieval annotates in-context ("unverified, from tool output").

Cap: no more than one memory-write LLM pass per session end. No per-turn extraction, ever.

### Retrieval

- **Always**: `MEMORY.md` index (≤200 lines hard cap; over-cap writes rejected with a "compact the index" error, à la Claude Code).
- **On demand**: `memory_search` tool → FTS5 `MATCH` with BM25 ranking, boosted by `0.995^(days since last_hit) + 0.1·log(1+hits)` (Generative-Agents-style recency/importance, no embeddings). Top-k=5, each hit stamped with its `source` and `last_hit` so the model can weigh trust and freshness; `last_hit`/`hits` updated on retrieval.
- Query expansion is unnecessary at this scale; LongMemEval's time-aware expansion matters only for 100k+-token histories, which mcode offloads to the episodic log, not the semantic store.

### Decay / forgetting

- **Supersession over deletion**: a new fact that contradicts an old one rewrites the topic file and marks the old entry `superseded_by` (cheap versioning — this is 90% of what Graphiti's bi-temporal graph buys, without the graph).
- **Decay**: a simple recency bucket (recent / stale / cold) applied as a BM25 tiebreaker, plus a monthly `mcode memory gc` that lists zero-hit, non-user-authored entries for one-keystroke bulk deletion. No continuous decay formula: at ≤200 index entries the ranking refinement is unmeasurable, and an unfalsifiable constant is worse than a coarse bucket.
- **Index pressure**: when `MEMORY.md` nears 200 lines, gc merges/drops stale entries first.

### User-inspectable

- Everything is plain markdown under the user config root (`24`) — greppable, diffable, editable in any editor.
- `mcode memory` TUI: list entries with source/age/hit count, edit, delete, purge a topic, toggle auto-memory off per project.
- Every automatic write shows a diff line in the session transcript. No silent writes.

### C++23 notes

- SQLite (with FTS5, bundled) is the only dependency; ~1 MiB. Single-writer WAL, opened per session.
- Index in memory: `MEMORY.md` is <25KB — parse once at startup, trivial.
- `std::filesystem` walks; dates via `std::chrono` (ISO 8601 in frontmatter, UTC).

## Traps

- **Auto-writing from every turn** — the mem0/A-MEM pattern. Costs ~1.2k–2.5k tokens + latency per op, floods the store with low-value entries, and mem0 still lost to full-context on LoCoMo accuracy. If a memory system's benefit doesn't survive an ablation, it's cost without function.
- **Silent memory writes.** Poisoning studies: average attack success rate 50.46% (and retrieval success 41.05%) against memory-writing agents; agents that "write and retrieve memory more aggressively are more exploitable"; prompt-injection defenses transfer poorly (best: PromptArmor 67.67% TPR; weak-signal attacks drop detection ~30–40pp) [arxiv.org/abs/2606.04329]. One poisoned write persists across sessions. Mitigations that follow directly: approval-gated writes, provenance tags, never writing untrusted tool output, source-aware retrieval annotation.
- **Stale memories outrank fresh code reality.** A remembered "test command" that changed last week actively misleads. Supersession + `last_hit` decay is mandatory, not optional.
- **Trusting vendor benchmark numbers.** mem0's current site claims (92.5% LoCoMo) come from a different pipeline than their paper's 66.88%; Zep's current page (90.2%, GPT-5.4 judge) is likewise non-comparable to their 2025 paper. And LoCoMo's answer key itself is disputed. Only compare within-paper ablations.
- **Vector DB as the default substrate.** For ≤10⁵ short notes, FTS5 BM25 is faster to build, needs no embedding model, and never goes stale on edit. Add sqlite-vec only if a measured recall gap appears.
- **Letting `MEMORY.md` grow unbounded.** Past the load cap, content silently vanishes from context — the agent then hallucinate-confidently "remembers" things it can no longer see.
- **Treating AGENTS.md as a correctness lever.** Evidence says it buys efficiency (fewer wasted test runs, fewer tokens), not pass rate. Write it short (≤200 lines); put multi-step procedures in skills instead.

## Open questions

- Does approval-gating measurably reduce memory usefulness vs Claude Code's silent writes? No public ablation; mcode should log proposal→accept/deny rates and revisit.
- Optimal episodic-log retention: full JSONL trajectories are cheap on disk but do they earn their retrieval cost vs. distilled failure notes only?
- Should subagents share the project memory index or get scoped views? (Claude Code: subagents don't inherit main-conversation memory.)
- Cross-machine sync of the user memory tree: plain-file git sync vs. deliberate non-feature?

## Sources

- https://arxiv.org/abs/2310.08560 — MemGPT (fetched)
- https://arxiv.org/abs/2504.19413 — mem0 paper (fetched via search results w/ numbers cross-checked)
- https://arxiv.org/abs/2501.13956 + https://blog.getzep.com/content/files/2025/01/ZEP__USING_KNOWLEDGE_GRAPHS_TO_POWER_LLM_AGENT_MEMORY_2025011700.pdf — Zep/Graphiti (fetched)
- https://arxiv.org/abs/2502.12110 + https://papers.neurips.cc/paper_files/paper/2025/file/19909c36f51abc4856b4560aff3d36d6-Paper-Conference.pdf — A-MEM (fetched)
- https://arxiv.org/abs/2304.03442 — Generative Agents (fetched)
- https://arxiv.org/abs/2405.14831 — HippoRAG (fetched)
- https://arxiv.org/abs/2410.10813 + https://github.com/xiaowu0162/LongMemEval — LongMemEval (fetched)
- https://arxiv.org/abs/2402.17753 + https://snap-research.github.io/locomo/ — LoCoMo (fetched)
- https://arxiv.org/abs/2507.05257 + https://github.com/HUST-AI-HYZ/MemoryAgentBench — MemoryAgentBench (fetched)
- https://arxiv.org/abs/2607.27250 + https://github.com/codeprakhar25/context-files-coding-agents — AGENTS.md/CLAUDE.md ablation (fetched via search results)
- https://arxiv.org/abs/2601.20404 — AGENTS.md efficiency study (fetched via search results)
- https://github.com/SaravananJaichandar/coding-agent-memory-benchmark — pre-registered SWE-bench memory study (fetched via search results)
- https://arxiv.org/html/2606.04329v1 — memory poisoning systematic study / MPBench (fetched)
- https://code.claude.com/docs/en/memory — Claude Code memory docs (fetched)
- https://cursor.com/docs/rules + memories docs — Cursor rules/memories (fetched)
- https://agents.md/ — AGENTS.md spec (fetched)
- https://www.sqlite.org/fts5.html — FTS5 (fetched via search results)
- https://github.com/asg017/sqlite-vec — optional vector extension (fetched via search results)
- https://github.com/neoneye/agent-memory-atlas (Engram entry) — file+FTS5 precedent (fetched via search results)
