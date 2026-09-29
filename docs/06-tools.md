# Tool Surface Design

> TL;DR: Ship **8 core tools** and make everything else a Lua extension, defer tier-2/3 tools behind a search meta-tool, keep tool results token-hostile-to-bloat, and treat code-mode as a later optimization — not the default path.

## State of the field

### Tool count vs accuracy

| Source | Finding |
|---|---|
| LongFuncEval (arXiv 2505.10570) | Catalog grown 49 → 741 tools (8K → 120K tokens of schemas): accuracy degraded **7.6%–85.6%** (relative) depending on model/split. Weak models lose 60–90%; GPT-4o lost ~5–21%. |
| RAG-MCP (arXiv 2505.03275) | Stress test 1 → 11,100 MCP servers: >90% selection success under ~30 candidates; collapses to **13.62%** accuracy with all schemas in prompt vs **43.13%** with retrieval (and 1,084 vs 2,134 avg prompt tokens). |
| OpenAI function-calling docs | "Aim for fewer than 20 functions at any one time" (soft guidance); hard limit 128 tools. [via Microsoft Research blog, 2025-09-11] |
| BoR paper (arXiv 2605.24660, Meta) | ~200 tokens per tool description; 100-tool shortlist ≈ 20K tokens before the query. On BFCL (370 tools), adaptive shortlist averaging **K=7.4** matched K=50 coverage (90.3% vs 90.8%); downstream Sonnet 4.6 selection 93.1% (adaptive) vs 87.1% (fixed K=5). |
| BFCL (Berkeley) | V2 Live averages ~3 candidate functions, max 37; no official accuracy-vs-tool-count curve exists. Claims of a fixed "%-drop per tool" are folklore. |
| tau-bench (Sierra) | Retail: 15 tools / 115 tasks; Airline: 13 / 50. Small curated catalogs; high failure rates come from interaction + policy, not tool count. |
| Arcade.dev (2025-12) | Independent test of Anthropic Tool Search with 4,027 tools, 25 trivial tasks: regex search 56%, BM25 64% correct-tool retrieval. `Gmail_SendEmail` and `Slack_SendMessage` were missed. |

Consensus: degradation is real, model-dependent, and driven as much by schema token volume and semantic overlap as by raw count. The only durable number is "keep the active set small"; the collapse threshold varies by model.

### Typed tools vs one general-purpose exec tool

- Claude Code ships ~15 typed tools plus `bash`; the typed tools (Read/Edit/Glob/Grep) exist because structured file tools get permission gating, diff rendering, and truncation control that raw shell cannot offer. [INFERENCE from public Claude Code behavior; not fetched this session]
- Anthropic "Writing effective tools for agents" (2025-09-11): consolidate workflows into fewer tools (`schedule_event` over `list_users`+`list_events`+`create_event`); namespacing (`asana_projects_search`) measurably changes selection accuracy; prefix vs suffix naming has "non-trivial effects" varying by model.
- Composio (via MSR blog): flattening nested parameter schemas improved tool-calling performance **47%** vs baseline.
- Microsoft MCP survey (1,470 servers, 12,643 tools): median schema depth 2, max 20; name collisions on 775 tools (`search` collides across 32 servers); 3,536 of 5,983 "successful" tool results actually contained errors in content with `isError=false`.
- Microsoft survey on response sizes: median tool output 98 tokens, mean 4,431, max 557,766; 16 tools overflow a 128K window in one call.

### Code mode / programmatic tool calling

| System | Mechanism | Reported numbers |
|---|---|---|
| CodeAct / smolagents CodeAgent (arXiv 2402.01030) | Model emits executable Python instead of JSON tool calls | 17 LLMs, 82 tasks: up to **+20pp** success and **30% fewer steps**; GPT-4: 74.4% / 5.5 turns (code) vs 52.4% / 7.6 (JSON) |
| Anthropic "Code execution with MCP" (2025-11-04) | MCP tools exposed as a file tree of typed stubs; agent writes code, explores FS for definitions on demand | 150,000 → 2,000 tokens (**98.7%**) on a Drive→Salesforce workflow; intermediate results never enter context |
| Cloudflare Code Mode (blog + Agents docs) | MCP schemas compiled to TypeScript declarations; code runs in an isolated Worker; `search()`+`execute()` for huge APIs | ~**81%** token reduction (client-side); Cloudflare API as 2 meta-tools: ~1,000 tokens vs ~1.17M (**99.9%**, vendor claim) |
| Anthropic Tool Search Tool (2025-11) | `defer_loading: true` + regex/BM25 search meta-tool; only matched schemas expand into context | ~77K → ~8.7K tokens (~85%); internal MCP eval accuracy: Opus 4 49%→74%, Opus 4.5 79.5%→88.1% (vendor-reported) |

Why code mode wins: (1) schemas load on demand from a filesystem, (2) loops/joins/filtering run in a sandbox so intermediate data never round-trips through the model, (3) control flow costs one generation instead of N turns.

Why it is not free: needs a sandboxed interpreter (security + binary size), error attribution gets harder (which line failed?), weaker models write worse code than they fill JSON schemas, and permission gating per-call becomes coarse (the gate sees one `code` call, not 5 tool calls). Cloudflare blocks direct network egress and keeps credentials host-side; Anthropic tokenizes PII in the client. Both add machinery.

### Tool result formats

- Anthropic writing-tools: `response_format: concise|detailed` enum; their Slack example used 72 tokens (concise) vs 206 (detailed) — ~1/3. Claude Code truncates tool responses at **25,000 tokens** by default.
- TOON (github.com/toon-format/toon): lossless JSON-to-table format; **~42.6%** fewer tokens on their mixed benchmark, ~43.6% on the example dataset; accuracy on their retrieval benchmark 72.2% (TOON) vs 71.4% (JSON). Wins only for large arrays of uniform objects; a 2026 benchmark (arXiv 2603.03306) found the advantage shrinks with the "prompt tax" in short contexts.
- MCP spec has no response-size guidance; MSR found the long-tail problem empirically (above).

### Errors as steering

- Anthropic writing-tools: error text should be actionable ("query must be ≤256 chars; you sent 400; retry with a shorter query") not stack traces or codes; truncation notices should redirect ("use offset=X to continue").
- MSR survey: servers routinely return error strings with `isError=false` — the model cannot distinguish failure from success. mcode MUST set a structured error flag.

## What works / what doesn't

**Works**
- Small always-loaded core (≤20 tools): consistent with OpenAI guidance and every strong production agent.
- Consolidated, workflow-shaped tools over CRUD mirrors: fewer schemas, fewer round trips, fewer wrong-parameter errors.
- Deferred loading for long tails: 85–99% schema-token reductions reported by three independent teams (Anthropic ×2, Cloudflare).
- Response shaping (pagination, field selection, truncation caps, concise/detailed): direct, measured token savings.
- Actionable error text: converts a failed turn into a self-correcting turn.

**Doesn't / unproven**
- Tool search at 4K tools: ~60% retrieval is not production-grade (Arcade). Vendors' internal-eval gains don't transfer to open registries.
- "Accuracy collapses after N tools" as a universal law: no clean curve exists (BFCL publishes none).
- TOON for everything: hurts on nested/irregular data and small payloads.
- Code mode as the only interface: sandbox cost, gating coarseness, and weak-model code quality are real; nobody has published a head-to-head on coding-agent tasks specifically.

## Recommended design for mcode

### Core tool set (C++)

**This table is canonical.** Other docs reference it; none redefine it (`03` §The tool registry).

The core ships only what the harness must **guarantee exists** or must **enforce itself**. Everything else — network, git, GitHub, skills, memory, plan tracking — is a first-party Lua extension (`23`), which is how the extension API gets dogfooded and how users opt out of capability they do not want.

Four tests decide core membership. A tool is core if it fails any of them:

| Test | Meaning |
|---|---|
| **Invariant-bearing** | It enforces something extension code cannot be trusted with (path boundaries, read-before-write hashing, sandbox gating) |
| **Trustworthy** | The harness must be able to trust its result and its accounting (budget, isolation) |
| **Bootstrapping** | It *is* the loading mechanism, or must exist before extensions can load |
| **Silent-failure** | Its absence causes the agent to **silently do the wrong thing** rather than the right thing more slowly |

That last test is the sharp one. A missing `task` tool means the agent does the work inline — slower, more context, still correct. A missing `ask_user` means the agent **guesses instead of asking**, and nothing surfaces that. Only the second is core.

| Tool | Class | Why core | Purpose |
|---|---|---|---|
| `read` | read | Invariant: workspace boundary, binary refusal, size caps | File read with offset/limit. **Files only** — URLs are `fetch`, an extension |
| `edit` | write | Invariant: read-before-write hash, anchor uniqueness | Exact-string replacement, anchor-matched |
| `write` | write | Invariant: read-before-write, size cap | Create/overwrite whole file |
| `glob` | read | Invariant: workspace boundary, expansion budget | Path patterns; never `ls` via shell |
| `grep` | read | Invariant: workspace boundary, result caps | Regex content search, gitignore-aware |
| `bash` | exec | Invariant: sandbox, permission gating, PTY | General exec; the composition escape hatch |
| `ask_user` | read | **Silent-failure** — its absence makes the agent guess | Ambiguity escalation (`11`) |
| `tool_search` | read | **Bootstrapping** — it *is* the loading mechanism | Find and load deferred tool schemas |

Eight core tools, **≈2–2.5K tokens** of schema (~200–300 each, BoR arXiv 2605.24660). Always loaded, cannot be disabled. Measured for the shipped eight: **4,297 bytes ≈ 1,074 tokens** (bytes ÷ 4), well under the 3K budget.

`glob` and `grep` are ignore-aware in the tool layer, not in `workspace`: `.git/`,
`.mcode/` and common build output directories are always skipped, and the root
`.gitignore` is honoured for its simple subset (literal names, `*.ext` wildcards,
trailing-`/` directory rules). Full gitignore semantics — nested files, negation,
`**` rules — are out of scope for the first batch; the workspace walk itself
stays ignore-blind.

The file primitives stay native rather than becoming extensions, even though they *could* be (Maki ships them as Lua). The reason is cooperation risk: a Lua `read`/`edit` would have to be trusted to call the host's read-recording API for the staleness check to work at all. Keeping them core means the file-integrity invariants have **zero cooperation requirement** — the same result with less surface area.

`task` follows the subsystem/tool-surface split instead: the **spawn primitives** (session, budget ledger, isolation, depth accounting) are core and trustworthy; the **`task` tool shell** is a load-bearing bundled extension (`23`). This matches the network and skills pattern, and it is what Maki does.

Deliberate omissions: no completion tool (the verification gate reads the final message plus harness-rerun exit codes — `11`); no `todo`, `git`, or network tools. Those are bundled Lua extensions (`23`). The C++ layer still owns egress policy, SSRF filtering, size caps, and untrusted-content delimiting, so a network extension cannot bypass the gate.



### File reading rules

`read` handles files only. What it does per input class is specified, because each case has a wrong answer that produces garbage tokens:

| Input | Behavior |
|---|---|
| Text file | Offset/limit, 1-based line numbers, default window **100 lines** (SWE-agent ablation: 100 beats both 30 and full-file) |
| Binary (NUL byte in the first 8 KiB, or known binary extension) | **Refuse with a stub**: `{path, size, detected_type}` plus a hint (`use bash with xxd/strings`). Never emit binary into context |
| Very large text (>1 MiB or >50k lines) | Serve the window; the stub reports total size so the model can navigate deliberately |
| Minified/generated (longest line >5 KiB, or generated-file marker) | Serve the window plus a `generated` note; suggest the source file |
| Wrong encoding | Decode as UTF-8 with replacement; report `encoding: non-utf8` rather than failing |
| Symlink | Follow, but resolve against the workspace boundary (`12`) — escaping symlinks are outside |
| Missing | Structured error with the closest existing path (typo recovery) |

**The three size caps agree by construction.** `read` serves a window through a
bounded streaming read (16 MiB per call) and never goes through `read_file`'s
whole-file path, so the 8 MiB `MAX_TEXT_FILE_BYTES` cap is a whole-file-consumer
bound, not a read-tool bound: a 9 MiB file is navigable a window at a time and
still writable (the 10 MiB `MAX_WRITE_FILE_BYTES` cap). A file above the write
cap is refused by `write` regardless of readability. The window threshold that
matters to the model is the 1 MiB `large_file` note, not a refusal.

`write` refuses to create files >10 MiB and refuses to overwrite a file it has not read this session (the read-before-write invariant). `edit` operates on exact anchors and never silently rewrites an entire file.

### Tool sources (the extensibility boundary)

Tools arrive from three interchangeable sources behind one interface, so the agent never learns a tool's origin:

| Source | Backing | Namespace | Cost |
|---|---|---|---|
| `BuiltinToolSource` | C++, compiled in | bare (`read`, `bash`) | zero runtime |
| `LuaToolSource` | extensions, loaded at startup (`19`) | `<ext>__<tool>` | one registry entry per tool; schema validated at registration |
| `McpToolSource` | MCP servers, connected at startup (`07`) | `mcp__<server>__<tool>` | process spawn; schema tokens |

Two rules make this work:

1. **The agent sees `ToolSpec`, never `ToolSource`.** Dispatch goes through the registry; a tool's provenance is invisible above that line.
2. **Registration is atomic per source.** A source that fails to load contributes zero tools, never a partial set — a half-registered extension is worse than a missing one.

Schemas from Lua and MCP are *untrusted input*: validate against the JSON-Schema subset at registration, reject malformed schemas rather than passing them to the model, and clamp declared permissions against config (`12`).

### On-the-fly tool creation (deferred)

Letting the agent write its own tools measured **+14pp on strong models and −30pp on weak ones**. It works only when the model reliably writes correct code; below that it manufactures failure modes. **Not in v1** — a Lua extension is the sanctioned way to add a tool. Revisit as a strong-model-only opt-in, gated by the eval suite.

### Loading strategy

The **effective** set is core + enabled extensions, resolved at session start and frozen for the session.

| Tier | Source | Default | Deferrable |
|---|---|---|---|
| 0 | Core (8 tools) | always | no |
| 1 | First-party Lua extensions (`23`) | enabled when their capability is configured | no — they are the default experience |
| 2 | Third-party extensions | opt-in per project, trust-gated (`19`) | no |
| 3 | MCP servers | opt-in per project | **yes** — `defer_loading` |

Budget: core ≈2–2.5K tokens; a typical effective set ≈3.5–4K. The `01` ≤3K budget applies to core; above 4K, tier-2/3 tools move behind `tool_search`.

`tool_search` is a core meta-tool (regex + name/description matching — BM25-class, no embedding model, no dependency). It returns name + one-line description; a second level expands the full schema. This is Anthropic's two-stage disclosure pattern without their API: ~100–200 tokens per matched tool instead of the catalog. Vendor guidance is to defer at >10 tools or >10K tokens of definitions.

**Deferral is cache-neutral.** Both Anthropic and [OI] append discovered schemas to *history*, leaving the tools array untouched (`15`). What breaks the cache is client-side mutation of the tools array — mcode never does that.

**Skills are not tools.** A skill is instructions loaded on demand (`08`); the boundary is: skills teach, tools act.

**Code mode is deferred.** It pays only when sessions chain ≥3 MCP calls or pass large intermediates; the sandbox is the hard part, and it is not a v1 feature.

### Tool result contract

- **Assume the agent never turns the page.** Measured: across 4,027-tool and document-retrieval studies, agents issued **zero** agent-initiated page-2 requests (arXiv 2608.26130). The harness — not the model — decides what is in the first chunk, and what matters is **coverage**, not ranking the single best hit: gold-anywhere-in-chunk is what drives success, and rank-1 precision does not. Front-load breadth; keyword scoring is sufficient; metadata signals actively hurt.
- **Default inline cap 2K tokens** (~8K chars); overflow spills to `.mcode/artifacts/`, returning a preview plus the path (`05` §Numeric guidance). **Absolute hard cap 25K tokens** per result ([CC]'s default) — beyond that, truncate even the preview with a resume hint (`"... truncated; re-call with offset=200"`).
- **Truncation notices are actionable**: name the filter, the smaller range, or the exact next call. Never a bare ellipsis.
- Edits return unified diffs, not whole files. Reads return only requested ranges.
- **Evidence loss is the hardest perturbation**: in a 14-subtype result-format benchmark, "return evidence loss" scored 0.142 — far harder than structural noise at 0.460. Reformatting a result is recoverable; dropping data from it is not. Prioritize not-dropping over prettiness.
- Errors: structured `{ ok: false, error: "<what failed>", hint: "<specific fix>", retryable: bool }` plus `isError` semantics that never lie. **59% of "successful" MCP results contained errors with `isError=false`** (MSR) — a model that cannot trust the success signal thrashes. Error text is prompt engineering; write it like advice to a colleague, including the fix.
- Uniform row data MAY be serialized compactly at the boundary; keep JSON internally. Token-optimized formats are not free inside loops: TRON costs −27% tokens at up to 14pp accuracy, and TOON collapses parallel tool-call output for most models (arXiv 2605.29676). Measure per payload.

### Tool authoring rules

Ranked by evidence strength (full derivation in the tooling research). These are review criteria for every tool, built-in or extension:

1. **Kill description defects first** — de-overlap sibling tools, remove implied prerequisites, keep parameter names in sync with the implementation *in the same commit*. Stale descriptions actively reverse gains.
2. **Make every error explicit, typed, and actionable.** Implicit failures are the expensive kind (recovery rate −37% under them); explicit errors are cheap to absorb (≤1.6% degradation at a 40% injection rate).
3. **Flatten parameter schemas; put defaults and bounds in each parameter description.** Flattening nested params measured **+47%**.
4. **Enforce structure at decode time, not by asking** — strict schemas or grammars. But note the format tax: imposing output schemas costs open-weight models ~3.9pp, so decouple reasoning from formatting.
5. **Bound results; assume no second page** (above).
6. **Right-size the read viewport** (100 lines) **and return summarized search hits** rather than raw matches (+6pp in the SWE-agent ablation).
7. **Guardrail the destructive tools.** Lint-on-edit converted a −7.7pp deficit into **+3.0pp** over an unguarded editor: the error text is part of the interface.
8. **Namespace with prefixes; keep the active set small and curated.** "Small" and "curated" are the joint answer to the count-vs-capability tension.
9. **Prefer workflow-shaped tools** over CRUD mirrors; consolidate chained calls; never make the model fill arguments the harness already knows.
10. **Return human-readable fields; hide IDs behind a `response_format` enum.**
11. **Add example *values* to parameter descriptions; avoid full example *calls*** — the latter measured harmful.
12. **Give short reasoning room before calls, not long.** 32 tokens of routing reasoning cut wrong-tool selection from 30.5% to 1.5%; 256 tokens induced hallucination.

Wall time: 0.18s

### Idempotency & permission gating

| Class | Tools | Gate |
|---|---|---|
| Read-only, local | `read`, `glob`, `grep`, plus read-class **extension** tools (`todo`, `skill_read`, `memory_search`) | auto-allow |
| **Network** | web_search, fetch | **ask** by default (per domain, per project), routed through the egress proxy (`12`). Auto-allowing network egress would defeat the proxy — an auto-approved `fetch` is an exfiltration channel by `12`'s own lethal-trifecta argument |
| Workspace-write | edit, write | auto within project root; prompt outside |
| Exec | bash | allowlist/denylist per session; prompt on first unmatched pattern; **persist the exact parsed argv** on "don't ask again" — never a wildcard prefix (`12` §Layer 1). No allow-by-prefix for exec-capable runners |
| Delegation | task | auto; inherits the spawner's gates |
| Deferred MCP | tool_search | search auto; execution gated per server/tool with per-tool annotations (destructive/open-world per MCP spec) |

Idempotency rules: read-class tools are trivially retryable. `edit`/`write` are retryable because they are content-addressed (re-applying the same old_string fails loudly instead of corrupting). `bash` is NOT assumed idempotent — the harness must not auto-retry exec-class calls; a retry policy belongs to the model, with the error text saying whether retry is safe.

### C++23 notes

- Tool registry: compile-time table of `{name, schema_json, handler, permission_class}`; schemas are static constexpr strings, zero runtime construction.
- Schema validation: single hand-rolled JSON-Schema subset validator (~basic types, required, enum) — no dependency; MCP schemas that exceed the subset fall back to permissive + model-side discipline.
- No interpreter in the core binary. Code mode, if adopted, links behind a feature flag so the default build stays lean.

## Traps

- **Schema bloat by accretion.** Every "handy" tool is ~200 tokens forever, on every request, for every model. Audit quarterly; delete tools that bash covers.
- **Believing vendor token-savings numbers as transferable.** 98.7% was one workflow; 81% was one experiment; 88.1% accuracy was an internal eval. Reproduce on your own workloads.
- **Tool search at huge scale without curation.** 60% retrieval at 4K tools means 4 of 10 tasks fail before selection. Curate registries; prefer 50 good tools over 4,000 reachable ones.
- **Nested parameter schemas.** 47% penalty for deep nesting (Composio). Flat params, unambiguous names (`user_id` not `user`).
- **Lying error flags.** MSR found 59% of "successful" MCP results contained errors. A model that can't trust success signals thrashes.
- **Code mode as a first feature.** It's a token optimization for MCP-heavy chains, not an architecture. Shipping the sandbox before the core loop works is inverted priority.
- **TOON everywhere.** Prompt tax and nesting make it a per-payload decision, not a default.
- **Name collisions across MCP servers.** `search` × 32 servers (MSR). Namespace everything (`server__tool`), collide-detect at connect time.

## Open questions

- Optimal Tier-0 size for coding agents specifically: is 10 right, or do read/edit/glob/grep merge further? Needs an internal eval sweep (1/5/10/20 tools) — no published curve covers coding-agent tool sets.
- Code-mode sandbox choice in C++23 (embedded interpreter vs child process vs Wasm) — security/perf/binary-size triangle unresolved.
- Does `tool_search` with local BM25 match Anthropic's hosted search quality at 100–500 tools, or only at small scale?
- Whether structured `isError` semantics alone reduce thrash measurably, or error *text quality* dominates.

## Sources

- https://arxiv.org/abs/2505.10570 — LongFuncEval (tool-count degradation numbers)
- https://arxiv.org/abs/2505.03275 — RAG-MCP (stress test to 11,100 servers; 13.62% vs 43.13%)
- https://arxiv.org/html/2605.24660v1 — How Many Tools Should an LLM Agent See? (BoR; ~200 tokens/tool; adaptive depth results)
- https://gorilla.cs.berkeley.edu/blogs/12_bfcl_v2_live.html — BFCL V2 Live (candidate counts)
- https://www.microsoft.com/en-us/research/blog/tool-space-interference-in-the-mcp-era-designing-for-agent-compatibility-at-scale/ — MCP survey (schema depth, collisions, response sizes, error-flag findings, OpenAI <20 guidance)
- https://www.anthropic.com/engineering/writing-tools-for-agents — tool design principles, concise/detailed (72 vs 206 tokens), 25K truncation cap
- https://www.anthropic.com/engineering/advanced-tool-use — Tool Search Tool (77K→8.7K tokens; Opus eval gains)
- https://www.anthropic.com/engineering/code-execution-with-mcp — code execution with MCP (150K→2K tokens)
- https://blog.cloudflare.com/code-mode/ and https://developers.cloudflare.com/agents/model-context-protocol/codemode/ — Code Mode (81%, 1.17M→1K, search/execute pattern)
- https://arxiv.org/html/2402.01030v4 — CodeAct (74.4% vs 52.4%; up to 30% fewer steps)
- https://www.arcade.dev/blog/anthropic-tool-search-4000-tools-test/ — independent 4,027-tool test (56%/64%)
- https://github.com/toon-format/toon and https://arxiv.org/abs/2603.03306 — TOON savings and the prompt-tax caveat
- https://sierra.ai PDF (tau-bench paper, via search) — 15/13 tool catalogs
