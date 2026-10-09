# mcode — Design Docs

> TL;DR: mcode is a lightweight, extensible C++23 CLI coding-agent harness with **Luau as its extension layer** — a native core for speed and a scripting ecosystem for reach. Read `01` for the thesis, `03` for the shape, `36` for the batch just shipped, then the topic you're building.
>
> **The extension VM is Luau**, chosen over LuaJIT on measured size, load time, RSS, and — decisively — a capability boundary LuaJIT cannot provide (`27`). `12` §Layer 3 owns what that boundary does and does not guarantee.

## What these docs are

Research + decisions for a greenfield coding-agent harness. Every claim is sourced; opinions are marked as decisions. No code — this is the *why* and the *what*, deliberately before the *how*.

The central design rule, from which most other decisions follow:

> **C++ provides the runtime primitives. Lua provides the ecosystem.**

Rules for this directory:
- One topic per file. Short, direct, no filler.
- `[UNVERIFIED]` marks anything not backed by a fetched source.
- Numbers carry their source; vendor claims are labeled `[VENDOR]`; third-party benchmarks `[THIRD-PARTY]`.
- If two credible sources disagree, both are shown and a side is picked with reasoning.
- Decisions are stated as decisions. "We chose X because Y" beats "X could be considered."

## Reading order

**Orientation**
1. `01-north-star.md` — thesis, hard budgets, principles, non-goals, success criteria.
2. `02-prior-art.md` — 16 harnesses compared, plus the Lua-extension precedent.
3. `03-architecture.md` — process model, layers, core types, extension seams, persistence.

**The loop and its context**
4. `04-agent-loop.md` — loop shape, state machine, termination, test-time compute.
5. `05-context-engineering.md` — budgets, compaction, cache alignment, context rot.
6. `11-reliability-and-evals.md` — grounding, anti-reward-hacking, eval harness.

**Capability**
7. `06-tools.md` — tool surface, count limits, code-mode vs tool-calls, MCP tool bloat.
8. `07-mcp.md` — MCP revisions, transports, client implementation, security.
9. `08-skills-and-agents-md.md` — SKILL.md and AGENTS.md standards, discovery, precedence.
10. `09-memory.md` — memory taxonomy, storage, write triggers, benchmarks.
11. `10-subagents.md` — fan-out rules, isolation, I/O contract, merge strategy.

**Extensibility (the differentiator)**
12. `17-lua-runtime.md` — the extension VM, its boundary, dialect, and embedding shape.
13. `18-lua-api.md` — the extension API surface, hooks, error conventions, trust tiers.
14. `19-extensions.md` — layout, manifest, lifecycle, tool sources, reload, distribution.
15. `20-events.md` — event representation, dispatch, threading, log coupling, Lua bridging.

**Prompting (what the model actually reads)**
16. `21-system-prompts.md` — prompt assembly, section order, evidence-backed rules, conflict linting.
17. `06-tools.md` §Tool authoring rules — the tool-description half of the same surface.

**Ecosystem**
18. `23-first-party-extensions.md` — the core/extension split, shipping extensions, the network split.
19. `25-distribution.md` — index-not-registry, trust model, manifest, docs site.

**Engineering**
20. `12-security.md` — threat model, permissions, sandboxing, the Lua trust boundary.
21. `24-cross-platform.md` — the seven OS interfaces, per-platform sandboxing, packaging.
22. `13-cli-and-tui.md` — render architecture, theme spec, library tradeoffs.
23. `14-cpp23-stack.md` — language features, library selection, build, binary size.
24. `15-model-layer.md` — provider abstraction, caching economics, model tiering.
25. `22-config-and-cli.md` — config scopes, CLI surface, exit codes, concurrency.
26. `16-roadmap.md` — milestones M0–M8, exit criteria, risk register.
27. `26-first-batch.md` — **the batch record**: the gate decisions, the measurement spine, and M0.
28. `27-a1-vm-spike.md` — the VM spike: measurements and the Luau decision.
29. `28-b-measurements.md` — the measurement spine: load, memory, dispatch.

**Parallel workstream plans** — three branches running at once off one base. Each
is a self-contained brief for one agent; none depends on another's code.

30. `29-workstream-model-client.md` — TLS, request rendering, the streaming client, retry.
31. `30-workstream-turn-loop.md` — the state machine, context assembly, compaction.
32. `31-workstream-core-tools.md` — the eight core tools, schemas, truncation, approval policy.

**Second batch — SHIPPED.** The three workstreams that make the agent usable on
a real repository: a permission engine with an approval prompt and a remember
store, `AGENTS.md` and skills, and a stdio MCP client. All three landed
together; `32` remains the plan of record and `33`–`35` the per-slice briefs.

33. `32-second-batch.md` — **the batch record**: permissions, skills, MCP, and Phase 0.
34. `33-workstream-permissions.md` — the permission engine, approvals, and the remember store.
35. `34-workstream-skills.md` — skills, the `AGENTS.md` instruction chain, prompt sections 10–11.
36. `35-workstream-mcp.md` — the stdio MCP client, tool registration, supervision.

**Third batch — shipped.** The three workstreams that turn a working batch tool
into a daily driver: an interactive TUI, the Lua API completed, and the OS
sandbox. Read `36` first; its §Outcome records what actually landed and the
three places the plan was wrong.

37. `36-third-batch.md` — **the batch record**: interactive surface, Lua API, sandbox.
38. `37-workstream-tui.md` — the terminal layer, cell renderer, streaming markdown, the REPL.
39. `38-workstream-lua-api.md` — the missing entry points, permissions, `/reload`.
40. `39-workstream-sandbox.md` — Low-integrity token, Landlock, Seatbelt, egress deny.

**Reference.** Read by number, never end to end.

41. `40-non-obvious-constraints.md` — the 127 traps that cost real time to find, numbered and stable. Docs and `AGENTS.md` cite them by number.
42. `41-session-analysis.md` — two live runs of one long task, dissected. Seven harness defects found by running, the model's measured behaviour, and the cache findings.

**If you only read three:** `01`, `03`, `21`.

> **Three batches have shipped.** `26-first-batch.md`, `32-second-batch.md` and
> `36-third-batch.md` are the records of what landed and why; `29`, `33`–`35`
> are the second batch's per-slice briefs and `37`–`39` the third's. `32` owns
> the permission defaults and `36` the sandbox's per-platform limits — read the
> owning doc before changing either.
>
> The deviations a reader would otherwise get wrong are in
> [`docs/40`](40-non-obvious-constraints.md) (items 48–67): workspace writes are
> `allow` by default, `--yolo` skips questions but not the hard-deny floor, and
> `--yolo` is not a sandbox.

## Decisions at a glance

| # | Decision | Doc |
|---|---|---|
| 1 | Single process; threads + one event loop. No coroutine graph, no daemon | `03` |
| 2 | Append-only JSONL event log is the source of truth; state is a projection | `03` |
| 3 | **Luau is the plugin ABI** — no C++ plugin interface, no `dlopen`. Chosen over LuaJIT on measured size, load, RSS, and boundary (`27`) | `01`, `17`, `19`, `27` |
| 4 | **No JIT in the extension VM.** Luau has no tracing JIT; the interrupt is the only execution control | `17` |
| 5 | **Extensions cannot reach the host except through the capability API.** Luau's boundary: no `io`/`package`, reduced `os`/`debug`, no bytecode, readonly globals, no `__gc`, host interrupt, memory ceiling. Not an OS sandbox and not formally proven | `12`, `17`, `27` |
| 6 | **Project extensions are inert until a hash-pinned trust grant** — never auto-execute on clone | `12`, `19` |
| 7 | One tool registry; built-ins, Lua, MCP, and skills all flatten into it | `03`, `06` |
| 8 | **8 core tools** (file primitives + `bash` + `ask_user` + `tool_search`); everything else is a Lua extension | `06`, `23` |
| 9 | Deferred tool loading (`tool_search`) once the tool budget is exceeded | `06` |
| 10 | Two-axis permissions: `sandbox` (OS-enforced) × `approval` | `12` |
| 11 | Stable prefix + append-only history + deterministic JSON for prompt-cache hits | `05`, `15` |
| 12 | Compaction at ~80% fill into `{summary, pinned_facts, decisions, open_questions}` | `05` |
| 13 | Artifacts on disk, referenced by path; content never inlined above a cap | `03`, `05` |
| 14 | One writer. Read-only fan-out only; writers need git-worktree isolation | `10` |
| 15 | AGENTS.md-compatible hierarchical instructions; SKILL.md progressive disclosure | `08` |
| 16 | Memory = inspectable markdown + FTS5; no embeddings in the hot path | `09` |
| 17 | Verification gate: harness re-runs the project's own checks before "done" | `11` |
| 18 | Custom ANSI renderer; inline transcript + bounded diffed live region | `13` |
| 19 | **Events**: closed tagged union, per-kind subscriber lists, sync dispatch, mutex queue | `20` |
| 20 | Extension API: ~18 entry points, integer handles, `value, err` / `error(msg, 2)` | `18` |
| 21 | Stack: **Luau**, yyjson, **boost::asio**, Beast, Boost.Process v2, fmt, spdlog, mimalloc, simdutf, unordered_dense, Conan 2 | `14` |
| 22 | **Size is the binding constraint** (~3–5 MB without TLS); dependency count is a review heuristic | `01`, `14` |
| 23 | Target MCP 2025-06-18 core with a `_meta` choke point for the stateless revision | `07` |
| 24 | **System prompt ~1,200 words**, named sections, one source of truth per rule, lint-gated | `21` |
| 25 | **Guardrails as concrete prohibitions** — the only reliably beneficial agent rules (+13.8pp) | `21` |
| 26 | **Read viewport 100 lines** (measured optimum vs 30 and full-file) | `05`, `06` |
| 27 | **Assume the agent never paginates** — the harness owns first-chunk coverage | `06` |
| 28 | **Deferred tool loading is cache-neutral** — schemas append to history, never splice the tools array | `15` |
| 29 | **Project config can only narrow** privileges, never widen | `12`, `22` |
| 30 | **Workspace = git root** (or explicit `--add-dir`); refuse write mode with no boundary | `12` |
| 31 | **Undo is a first-class mechanism** — git checkpoint or shadow store, never just event replay | `03` |
| 32 | **Reasoning effort scheduled per loop phase** (high on plan/verify, lower mid-implementation) | `15` |
| 33 | **No repo map in the prompt** — most expensive retrieval strategy tested, no positive delta | `05` |
| 34 | **No telemetry in v1**, and never by default | `22` |
| 35 | **Windows, Linux, macOS are all tier-1**; seven OS interfaces from M0 | `24` |
| 36 | macOS **cannot be fully static-linked** — promise is "no third-party runtime deps", not "one file" | `24` |
| 37 | **All tools except file/exec primitives ship as Lua extensions** — network, git, skills, memory, plan tracking | `06`, `23` |
| 38 | Network extensions are **policy-free** — egress, SSRF, caps, delimiting stay in C++ | `23` |
| 39 | `task` splits: spawn primitives core, tool shell a load-bearing bundled extension | `23` |
| 40 | **Index, not registry** — no artifact hosting, no accounts; install pins a git SHA in a lockfile | `25` |
| 41 | **Open publishing + automated screening, not review-gating** | `25` |
| 42 | Never auto-update extensions; show the diff and require confirmation | `25` |
| 43 | **TOML everywhere** — config, extension manifests, registry metadata | `22`, `25` |
| 44 | Permission decision = session policy(class, path/argv) ∩ extension manifest — more restrictive wins | `03`, `12`, `23` |
| 45 | **No completion tool**; "done" = model ends its turn, and the harness runs a **config-owned** verification command (never model-chosen) | `06`, `11` |
| 46 | Extension disablement: any scope may disable; only user scope may enable; project cannot disable `task` without an explicit opt-in | `22`, `23` |
| 47 | Prompt sections are assembled from the **effective tool set** — a rule referencing an unloaded tool is omitted, never dangling | `21` |
| 48 | Repo-local extensions are **semi-trusted after a hash-pinned grant** (in-process, same VM boundary as trusted); the out-of-process untrusted tier is a v1 non-goal | `12`, `18` |
| 49 | Sessions **branch by lineage**, not by copying the log; `seq` stays global and monotonic | `04`, `20` |

## Known disagreements (deliberately unresolved or resolved-by-argument)

| Question | Positions | Where |
|---|---|---|
| Multi-agent: does it work? | Cognition: no parallel writers. Anthropic: +90.2% on research. **Resolution:** read vs write split. | `10` |
| Bash-only vs typed tools | mini-SWE-agent: >74% SWE-bench with bash only. Everyone else: typed tools. **Resolution:** typed core + bash tail. | `02`, `06` |
| Code-as-action vs JSON tool calls | CodeAct: +20pp, 30% fewer steps. Native tool-calling improved since. | `06` |
| Grep/glob vs embeddings for code retrieval | Anthropic: agentic search. RAG camps: embeddings. Evidence is mixed. | `05`, `09` |
| Agentless vs agentic | Agentless: cheaper, competitive in 2024. Agentic won on hard tasks. | `04`, `11` |
| ~~LuaJIT vs Luau~~ | **Resolved 2026-09 in favour of Luau** — measured faster on load and RSS, and the only one of the two with a capability boundary. `27` | `17`, `27` |
| **JIT on or off** | JIT: 2–15× on numeric loops. Measured 2.1× *slower* on C-boundary-heavy glue, and instruction hooks do not fire under JIT. **Resolution:** off by default, on per-extension. | `17` |
| **Core vs extension line** | Maki ships all 21 builtins as Lua plugins; Neovim ships LSP in Lua; VS Code ships ~90 built-in extensions. Against: Zed forbids extensions from adding agent tools; SWE-agent's authors deprecated their own custom ACI. **Resolution:** invariant-bearing primitives stay core; everything user-facing is an extension, via four explicit tests. | `06`, `23` |
| **Review-gated vs open registry** | Zed/Raycast/Homebrew gate on human review; Obsidian abandoned it after admitting updates were never re-reviewed. **Resolution:** open publishing + automated per-version screening + runtime trust. | `25` |
| **In-process vs out-of-process extensions** | Neovim/mpv/WezTerm run in-process. VS Code uses a process; Zed uses WASM. **Resolution:** in-process for trusted, subprocess only if an untrusted tier ships. | `12`, `19` |

## Conventions

- **Terminology** is fixed in `01-north-star.md`; use those words exactly.
- **Budgets** (startup, memory, tokens, binary size) are defined in `01` and referenced, never redefined.
- **Tool names** in `snake_case`; Lua extension tools `<ext>__<tool>`; MCP tools `mcp__<server>__<tool>`.
- **Paths** in docs use POSIX form; Windows equivalents implied.
- **Sources** sections list URLs actually fetched. Anything else is `[UNVERIFIED]`.

## Status

| Area | State |
|---|---|
| Docs `00`–`41` | Written. `40-non-obvious-constraints.md` is the numbered trap reference; `41-session-analysis.md` is the live-run analysis. |
| Code | Three batches shipped: agent loop, permissions, skills, MCP, TUI, Lua API, sandbox, sessions. |
| Not built | Memory, subagents, OSC 52 clipboard |

## Open questions (cross-cutting)

- Does the speed/lightweight advantage translate into user-visible value, or is it a technicality? (`01`)
- Can a small team sustain C++23 velocity against Rust/TS harnesses? (`14`)
- Does the Lua extension ecosystem bootstrap, or do extensions stay a power-user feature? (`19`)
- Which decision is most likely wrong in 12 months? Candidates: #18 (custom renderer), #23 (MCP revision pin), #9 (deferred tools), #4 (JIT off).

## Sources

This index synthesizes the docs in this directory; each carries its own primary sources.

- https://www.anthropic.com/engineering/building-effective-agents — agent vs workflow framing
- https://cognition.com/blog/dont-build-multi-agents — the multi-agent counter-position
- https://github.com/swe-agent/mini-swe-agent — minimal-harness baseline (>74% SWE-bench Verified)
- https://agents.md/ — AGENTS.md adoption
- https://www.anthropic.com/engineering/equipping-agents-for-the-real-world-with-agent-skills — SKILL.md progressive disclosure
- https://research.trychroma.com/context-rot — context degradation with input length
- https://manus.im/blog/Context-Engineering-for-AI-Agents-Lessons-from-Building-Manus — KV-cache discipline
- https://luau.org/ + https://luau.org/sandbox/ — Luau overview and the capability boundary
- https://github.com/luau-lang/luau — source, CMake build, `lua_resetthread`
- `docs/27-a1-vm-spike.md` — the spike that chose it
- `docs/28-b-measurements.md` — load, memory, and dispatch measurements
- https://maki.sh/docs/plugins/ — Lua-extension precedent in a Rust coding agent
- https://github.com/kfcafe/imp — Lua-extension precedent
