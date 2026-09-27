# North Star

> TL;DR: mcode is a single-binary, native C++23 coding agent whose only defensible advantages are *speed*, *context discipline*, and *verifiable correctness* — everything else is copied from proven harnesses rather than reinvented.

## Why this exists

The evidence is unambiguous: every serious coding harness converged on the same skeleton — a single LLM loop over a small set of typed file/shell tools, OS-level sandboxing, markdown instruction files, MCP for extension, subagents for context isolation (`02`). Divergence is in context management, permissions UX, and terminal quality.

So "another harness" is a losing frame. Four frames are winnable:

1. **Native speed as a product.** Every comparable harness ships a runtime: Node, Python, Go, or Rust. A statically linked C++23 binary can cold-start in single-digit milliseconds with a small resident set. That is not a benchmark curiosity — it is what makes the tool composable (scriptable, pipeable, spawnable in parallel, embeddable in CI) instead of a long-lived session you keep open. Nobody in this space currently optimizes for it. The measurement that makes this concrete: Pi (TypeScript) is 590.7 ms to first frame and 144.4 MB PSS; a native Rust harness measures 14.0 ms and 27.8 MB `[THIRD-PARTY]` (`02`).
2. **A scripting ecosystem in a native core.** No mainstream harness ([CC], Codex, opencode, Pi, Goose) embeds a scripting language — their extension language *is* their runtime. Embedding a scripting VM means the extension ecosystem costs under 1 MB and nothing at all when unused, and extensions are written without a compiler (`17`). Two small Rust harnesses (Maki, imp) already prove the model; nobody serious has done it in C++.
3. **Context as a budgeted resource.** Models degrade before their context window fills; shorter, higher-signal prompts outperform long histories on recall tasks, and prefix instability destroys prompt-cache economics (`05`, `15`). Treat context as a budget with explicit accounting, not a buffer you append to.
4. **Trust over raw capability.** On SWE-bench Verified the top tier is saturated and statistically hard to separate (`11`). The remaining headroom is not resolve rate; it is whether the agent can be trusted not to game the grader, silently shrink scope, or claim success it never verified. Grounding and verification are where a harness can still differentiate.

Everything else — tool names, loop shape, permission axes, TUI idioms — is deliberately copied from systems that already validated it.

## Constraints (hard budgets)

| Constraint | Budget | Rationale |
|---|---|---|
| Cold start | ≤ 15 ms | Enables per-invocation spawning, pipes, CI use |
| Resident memory, idle session | ≤ 30 MB | Parallel fan-out without swapping the machine |
| Binary size, TLS included | ≤ 25 MB (target ~5–9 MB) | **No third-party runtime dependencies on any platform.** Fully static on Windows (`/MT`) and Linux (musl); macOS cannot be fully static — `libSystem` stays dynamic by OS requirement (`24`) |
| Direct third-party dependencies | ≤ 12, with **size as the binding constraint** | Each dep is a build, audit, and size liability; the count is a review heuristic, not the goal |
| Always-loaded tool schema | ≤ 3K tokens (core ≤2.5K; ≤3.5K with default extensions) | Tool-set size degrades selection accuracy (`06`) |
| Harness-added time-to-first-token | ≤ 5 ms | The harness must never be the latency bottleneck |
| Extension load cost | **linear, ≤1 ms for the first, ≤55 µs each after** | Registration only; work is deferred (`19`). Measured: 0.37 ms for 1, 2.72 ms for 50 (`27`) |

These are engineering constraints, not aspirations. A change that breaks one is a regression.

**On the dependency budget.** The original ≤6 was a proxy for "stay small" before the stack was chosen. The concrete stack (`14`) lands at ~10 direct dependencies and an **estimated** 2.9–5.0 MB without TLS, 5–9 MB with it (cross-source planning estimates, not measurements — validate with a link map) — comfortably inside the size budget. So **size is now the real constraint** and the dependency count is a review heuristic: a new dependency must justify itself against measured size and compile-time cost, not against an arbitrary integer. Test-only and vendored single-header code never counted and still do not.

## Principles

1. **The loop is the product; everything else attaches to it.** Core = agent loop + context manager + tool dispatcher + provider client. MCP, skills, hooks, and subagents are plugins on one registry.
2. **One dispatch table.** Core tools, first-party and third-party Lua extensions, and MCP servers all flatten into a single namespaced registry with uniform schemas, permission classes, and result types. The agent's *dispatch path* cannot tell where a tool came from — that is what keeps the core small. Provenance is still recorded and surfaced **to the user** (logs, notifications, veto reasons, `ext doctor`), because attribution is what makes an ecosystem debuggable (`06`, `23`).
3. **Bash is the escape hatch, not the foundation.** Typed file primitives for the common cases; one general shell tool for the tail. Bash-only harnesses prove you can be minimal; typed tools prove you should be precise where it matters.
4. **Enforce invariants in the harness, not the prompt.** Read-before-write, anchor-matched edits, verification before "done". Prompts are advisory; the tool layer is not.
5. **Everything is inspectable and replayable.** Append-only event log as the source of truth; typed events; artifacts on disk referenced by path, never inlined wholesale.
6. **Fail closed on authority, fail open on capability.** Unknown permission → deny and ask. Untrusted extension (repo-shipped, ungranted) → **do not load it**. Extensions are trusted code or they do not run in-process (`12`).
7. **Cache-aligned prompt construction.** Stable prefix, append-only history, deterministic serialization. Layout discipline is an economic requirement, not a style preference.
8. **Luau is the plugin ABI.** The C++ core exposes a small stable API; extensions are Luau. This is the central design rule: *C++ provides the runtime primitives, Lua provides the ecosystem.* No C++ plugin interface, no `dlopen`, no hand-maintained C ABI — the ABI is a scripting language, which is versionable and writable without a compiler. The VM is a **designed, in-process capability boundary that reduces blast radius** — not an OS sandbox, not formally proven, and never described as a sandbox in user-facing text (`12`, `17`, `27`).
9. **Cross-platform is a constraint, not a port.** Windows, Linux, and macOS are **all tier-1**: first-class sandboxing, PTY, packaging, and signing on each. Every abstraction that touches the OS (process spawn, PTY, sandbox, paths, file watching) is designed behind an interface from M0 — retrofitting one platform later is how a project ends up with a Windows-shaped core (`24`).
10. **Boring technology.** Boring dependencies, boring concurrency, boring storage. Novelty budget is spent on the differentiators above, nowhere else.
11. **Measure or it did not happen.** Every prompt, tool, or context change is evaluated against a fixture suite before it is believed (`11`).

## Non-goals

- **Not a framework or SDK.** A CLI. No embedding API in v1.
- **Not a multi-agent swarm.** One writer. Read-only fan-out only (`10`).
- **Not an IDE.** Editor integration via protocol, not a GUI.
- **Not a model host.** No local inference; local models are just another HTTP endpoint.
- **Not an OS sandbox implementation.** Uses OS primitives on all three platforms and can *drive* your container runtime as the paranoid tier; it ships no OS sandbox of its own. The extension VM's capability boundary is a separate, in-process control — see principle 8 (`12`, `24`).
- **Not a vector database.** Retrieval is agentic search first; lexical index only if measured necessary.
- **Not permission-free.** Bypass modes exist and are loud; the default is sandboxed with approval on egress.
- **Not a model-quality play.** We win on harness quality at equal model.

## Success criteria (v1.0)

| Criterion | Target | Measurement |
|---|---|---|
| Cold start, warm cache | ≤ 15 ms | `hyperfine`, ≥1000 runs |
| Harness-added TTFT overhead | ≤ 5 ms | Instrumented, versus raw request |
| Static binary size | ≤ 25 MB | Stripped, LTO, measured |
| Idle RSS | ≤ 30 MB | Resident set after session open |
| Fixture-suite pass rate | ≥ parity with the reference minimal harness at equal model | Local eval suite (`11`) |
| Undetected reward hacks in fixture suite | 0 | Hack-flag assertions (`11`) |
| Add capability without recompiling | 100% | Bundled extension, MCP server, skill, or hook — all runtime-configurable |
| New provider adapter | one file, **zero recompilation** | A Lua descriptor plus C++ codec (`26` D2) |

## Vocabulary (fixed)

Use these terms exactly, in docs and code:

| Term | Meaning |
|---|---|
| **Harness** | The whole program: loop + context + tools + providers |
| **Session** | One conversation; append-only event log on disk; resumable |
| **Turn** | One model request/response cycle |
| **Step** | One tool call plus its result |
| **Run** | One top-level user request, spanning many turns |
| **Subagent** | An isolated agent session spawned by the `task` tool |
| **Skill** | A `SKILL.md` directory: instructions plus optional scripts |
| **Extension** | Anything added without recompiling: **a Lua extension**, MCP server, skill, or hook |
| **Tool** | A callable with a JSON schema in the unified registry |
| **Artifact** | A file under `.mcode/artifacts/<run-id>/`, referenced by path |
| **Compaction** | Replacing history with a structured summary plus pinned facts |

## Open questions

- Is native speed *felt*, or only measurable? A 15 ms start on a 30 s task is invisible. The bet is that it matters for scripting, fan-out, and CI — this needs user evidence, not assertion.
- Can a small team sustain C++23 velocity against Rust and TypeScript harnesses with larger contributor pools?
- Does verification-first design cost more in throughput than it returns in trust? Every gate is a turn; this needs ablation, not intuition.

## Sources

- https://www.anthropic.com/engineering/building-effective-agents — workflow vs agent distinction; "start simple, add autonomy only when it measurably helps"
- https://cognition.com/blog/dont-build-multi-agents — single-threaded default; actions carry implicit decisions
- https://arxiv.org/abs/2609.17394 — SWE-bench Verified top tier statistically indistinguishable (76.4–79.2%)
- https://www.swebench.com/ — leaderboard; mini-SWE-agent as reference scaffold
- https://github.com/swe-agent/mini-swe-agent — bash-only minimal loop as a strong baseline
- Research docs `02`–`16` in this directory (each carries its own primary sources)
