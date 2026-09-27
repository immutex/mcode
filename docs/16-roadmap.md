# Roadmap

> TL;DR: Ship a boring, correct, fast loop first (M0–M4); make Lua the extension seam at M5 and prove it by moving real features into Lua (M6–M7); then earn the word "reliable" with evals and verification gates (M8+). No milestone ships a feature that has not been smoke-tested against a real repo.

## Sequencing principles

1. **Vertical slices, not layers.** Every milestone produces a runnable binary that does one more real thing end-to-end.
2. **Loop before surface.** A headless loop that works beats a beautiful TUI over a broken loop.
3. **Lua late, but designed for from day one.** The extension API is the differentiator, but an extension API over an unfinished core is churn. Every layer before M5 is built so its seam is a registry or an event, never a direct call.
4. **Evals before claims.** The eval harness lands in M4, before most features, so every later change is measured. Retrofitting evals is how projects die.
5. **All three platforms from M0.** Windows, Linux, and macOS are tier-1. Cross-platform is not a port; it is a constraint on every abstraction (process spawn, PTY, sandbox, paths, file watching). Retrofitting a platform is how a core ends up Windows-shaped (`24`).
6. **Prove Lua by dogfooding.** M7 builds real capability *only* in Lua — workflow tools (git commit conventions, GitHub flows) that were deliberately kept out of the C++ core. If the Lua API cannot express them, the API is wrong.
7. **No milestone is "done" without a smoke run** against a fixture repo and a recorded transcript.

## Milestones

### M0 — Skeleton (weeks 1–3)
**Goal:** `mcode "explain this repo"` streams a model response and exits.

| Deliverable | Notes |
|---|---|
| Build system | CMake + Ninja + Conan 2 (`CMakeToolchain`/`CMakeDeps`), lockfiles committed; LTO, gc-sections; **all three platforms in CI from day one** — MSVC (`/MT` static), GCC/Clang + musl static, Apple Clang (`24`) |
| Platform shims | The seven OS seams (`24`) with at least stub implementations on each platform: `PtySession`, `Sandbox`, `Termination`, `fs` helpers, `paths`, `ResizeSource`, `FileWatch`. Stubs are fine at M0; the *interfaces* are not |
| Release pipeline | Per-platform artifacts + signing: Authenticode on Windows, ad-hoc `codesign` on macOS arm64 (re-sign after strip), musl tarballs on Linux (`24`) |
| Provider client | One streaming provider over Asio/Beast, robust SSE line parser, retry/backoff, token accounting |
| Message/content-block types | Provider-neutral; one adapter file |
| Session event log | Append-only JSONL, flush per event, replay |
| Event bus v1 | Closed tagged union, per-kind subscriber lists, sync dispatch, mutex queue (`20`) |
| Headless surface | `mcode exec --json`, stdout streaming, exit codes |
| Config loading | TOML; global + project precedence; env overrides |
| Fixture repo + 10 smoke tasks | Deterministic; used by every later milestone |
| **Eval run-record schema + pass@k machinery** | The JSONL record from `11` and the aggregation, wired to the smoke tasks. This is pulled forward deliberately: M1's exit criteria are unverifiable without it, and `16`'s own principle is "evals before claims" |

**Exit criteria:** cold start ≤ 15 ms; a 10-task smoke suite runs unattended; crash mid-run replays cleanly.

### M1 — Tools and the loop (weeks 4–8)
**Goal:** the agent can read, search, edit, and run commands in a repo.

| Deliverable | Notes |
|---|---|
| Core tool set | `read`, `edit`, `write`, `glob`, `grep`, `bash` — typed schemas, ~3K token budget (`06`). **No `git` tool**: git is bash one-liners, and a workflow-shaped `git_commit` is exactly the kind of tool that belongs in a Lua extension (`06` §Core tool set) |
| Tool registry + `ToolSource` abstraction | One table, permission classes, collision checks; **the seam Lua and MCP will plug into**. Tools load conditionally on capability (`06`), so `task`/`tool_search`/`skill_read`/`memory_search` are simply absent until their subsystem exists — no dead schema tokens |
| Two-tier model routing | `act` + `side` tiers wired from the start (one config field, one request param). M2's compaction summarizer needs the cheap tier, and deferring it to M8 is a cost regression the docs warn against |
| Read-before-write invariant | Content-hash staleness check; anchor-matched edits |
| Tool result truncation + artifacts | Per-tool caps; spill to `.mcode/artifacts/` |
| Sandbox v1 | **Interfaces on all three platforms; one strong implementation.** Linux Landlock (runtime ABI detection) + seccomp ships fully here; Windows and macOS get the `Sandbox` interface plus **fail-closed approvals** as the boundary. Shipping three production sandboxes — one of them via an undocumented macOS API — in a 5-week milestone is not credible. Full three-platform enforcement lands in M6 alongside the egress proxy |
| Egress deny (minimal) | A deny-by-default egress rule per platform (WFP / Landlock TCP / Seatbelt `deny network*`) even before the proxy. `12` calls egress the highest-value control; M1 should not ship networking with no OS-level block |
| Permission UX | `sandbox × approval` two-axis model (`12`) |
| Loop guards | Turn/step/token budgets, thrash detection, terminal-state machine (`04`) |
| Subprocess handling | Boost.Process v2, async pipe pumping, close-stdin discipline (`14`). `posix_spawn`/`CreateProcess` only — never `fork` (`24`) |
| **Platform interfaces** | The seven OS seams (`24`): `PtySession`, `Sandbox`, `Termination`, `fs` helpers, `paths`, `ResizeSource`, `FileWatch`. Established in M0 so no platform check ever leaks into portable code |

**Exit criteria:** agent completes 10 real bugfix tasks in the fixture repo; zero unrecoverable permission bypasses; 100% of tool errors surface honestly.

### M2 — Context discipline (weeks 9–11)
**Goal:** long sessions do not rot.

| Deliverable | Notes |
|---|---|
| Context manager | Budgeted assembly, cache-breakpoint placement, byte-stable prefix (`05`) |
| Compaction | Threshold-triggered (80%), structured `{summary, pinned_facts, decisions, open_questions}` |
| Instruction files | `AGENTS.md` hierarchical discovery + merge, token budget enforcement (`08`) |
| Tool-result clearing | Restorable stubs, `clear_at_least` discipline (`05`) |
| `todo` recitation | Goal restated near the end of context every N turns |
| Cost/token meter | Live accounting, per-turn and per-run |

**Exit criteria:** a 200-turn session completes without context-related failure; prompt-cache hit rate is measured and reported.

### M3 — Sessions and reliability (weeks 12–15)
**Goal:** sessions are durable and the agent can be trusted.

| Deliverable | Notes |
|---|---|
| Session resume/replay | Restore from JSONL; branch and checkpoint semantics |
| Snapshot events | Periodic state snapshots for long sessions (`20`) |
| Verification gate (mechanism) | Harness re-runs the project's own tests/build; failure blocks "done" (`11`). Tuning the catch-rate target waits for M4 |
| Anti-reward-hacking checks | Test-file diff audit, original-test-hash requirement, grader isolation |
| Claim→evidence ledger | Edits cite the read that justified them |
| Scope-drift detection | Diff vs stated plan; escalate on divergence |
| Ambiguity escalation | Ask instead of guessing on materially different readings |

**Exit criteria:** zero **confirmed** reward hacks across the eval suite (absence of detection is not absence of hacks — the pipeline distinguishes flagged from confirmed, `11`); the verification gate's catch rate is measured, with ≥90% as the target to tune at M4.

### M4 — Evals (weeks 16–17)
**Goal:** every later change is measured.

| Deliverable | Notes |
|---|---|
| Fixture-repo eval suite | 30–100 deterministic repos, seeded, offline, <60 s each (`11`) |
| JSONL run records | Full schema from `11`: outcome, metrics, grounding, hack flags, verifier |
| pass@k / pass^k reporting | Capability and reliability reported separately |
| CI integration | Suite runs on every prompt/tool/model change |
| Trajectory evals | Score the process, not just the outcome |

**Exit criteria:** the suite runs in CI in <10 min; a deliberate prompt regression is detected.

### M5 — Lua runtime (weeks 18–22)
**Goal:** a Lua script can extend the harness without recompiling it.

| Deliverable | Notes |
|---|---|
| LuaJIT vendored + CMake shim | Commit-pinned `v2.1`; `msvcbuild.bat` on Windows (`17`) |
| VM lifecycle | `lua_newstate` with allocator routed to mimalloc; **JIT off by default** |
| Extension loader | Discovery, manifest validation, `api_version` check, restricted env (`19`) |
| Core API surface | ~18 entry points from `18`: `tool.register`, `cmd.register`, `on`/`off`, `emit`, `defer`, `timer`, `log`, `cfg`, `spawn`, `fs`, `session.snapshot` |
| `LuaToolSource` | Lua tools land in the same registry as built-ins |
| Event→Lua bridge | Classic `lua_CFunction` on the loop thread, `pcall` per hook, veto convention (`20`) |
| Error containment | pcall boundaries, per-extension error counter, quarantine |
| `/reload` | Fresh `lua_State` per extension; stale-closure semantics documented |
| **First bundled extensions** | `task`, `fs-extra` (`todo`), `skills`, `memory` — the ones whose subsystems already exist. They land **in M5, after the loader**, not later: they are the dogfooding that proves the API before the ecosystem depends on it. Exit criterion: **the disable-all test** (`23`) — every first-party extension disabled produces a working, less capable agent. If it does not, something became core by accident |

**Exit criteria:** an extension registers a tool and a command; the agent calls it with no knowledge it came from Lua; a deliberately broken extension is quarantined without killing the session.

### M6 — Trust and extensions (weeks 23–25)
**Goal:** extensions are safe to *distribute*, not just to write.

| Deliverable | Notes |
|---|---|
| Trust grant UX | View-then-grant, hash-pinned, per-file; persisted trust DB (`12`) |
| Project-extension gating | `.mcode/extensions/` inert until granted; `--no-extensions` for CI |
| Manifest permissions | `fs_read`/`fs_write`/`spawn`/`net` declarations enforced on the API surface |
| `mcode ext doctor` | Per-extension health, error counts, permission audit (`19`) |
| Disable list | Config-level and runtime disable without file edits |
| Sandbox hardening | **Full three-platform enforcement**: Windows restricted token + Job Object; macOS Seatbelt via `sandbox_init_with_parameters` (deny-default profile). Plus the egress proxy with domain allowlist, env scrubbing, UNC path prompts |

**Exit criteria:** a cloned repo containing a malicious extension cannot execute code; the grant flow shows exactly what will load.

### M7 — Skills, MCP, and dogfooding (weeks 26–30)
**Goal:** the remaining subsystems exist, and Lua has displaced real C++ features.

| Deliverable | Notes |
|---|---|
| Skills | `SKILL.md` discovery, progressive disclosure, `/skill`, precedence (`08`) |
| MCP client | stdio first, then Streamable HTTP; `server/discover`; tool namespacing (`07`) |
| `McpToolSource` | MCP tools land in the same registry; deferred loading behind `tool_search`. Discovery is config + `initialize` + `tools/list` for the pinned revision — **not** `server/discover`, which is a 2026-07-28 RPC (`07`) |
| MCP in Lua | Extensions can declare and configure MCP servers |
| **Remaining bundled extensions** | `web` (network), `git`, `github`. With the M5 set these complete the shipping catalog (`23`). The strongest API test: if a Lua extension cannot express a workflow tool composing `bash`, the API is wrong |
| **Dogfood: `github` extension** | Real functionality that was never going to be in the core |
| TUI | Damage-tracked renderer, streaming markdown, diff rendering, theme (`13`) |

**Exit criteria:** the Lua git and GitHub extensions deliver capability the C++ core never had, at zero schema cost until invoked; adding an MCP server + a skill requires zero code changes; the tool block stays under budget with 5 MCP servers connected.

### M8 — Reach (weeks 31+)
**Goal:** fit into real workflows and grow the ecosystem.

| Deliverable | Notes |
|---|---|
| Additional providers | Anthropic, Gemini, local (Ollama/llama.cpp) — one adapter file each |
| Prompt-cache optimization | Per-provider breakpoint strategies; measured hit rate (`15`) |
| Model tiering | Cheap model for compaction/titling/search; frontier for planning/edits |
| Subagents | Read-only fan-out, one-shot specs, artifact spillover (`10`) |
| Memory | `MEMORY.md` + FTS5 index, approval-gated writes (`09`) |
| ACP / editor integration | Protocol, not a GUI |
| Distribution | Signed static binaries, package managers, checksums |
| **Registry + install** | The index, `mcode ext install`, the **lockfile format**, and the update UX (`25`). These are declared *irreversible* choices (`25` §Versioning), so they need a milestone, not just a design |
| Extension gallery | The curated index and report flow; **no untrusted tier** (`25`) |

## Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| LuaJIT bus factor 1 (Mike Pall) | Supply risk | Pin a commit; vendor the source tree; OpenResty's synced downstream is the fallback (`17`) |
| Extension ecosystem never bootstraps | The differentiator is unproven | Dogfood at M7 by building real capability only in Lua; if the API cannot express it, fix the API — do not add C++ features back |
| Lua extensions mistaken for a sandbox | Security incident | Docs state the trust model plainly; project extensions inert by default; the word "sandbox" never appears next to "Lua" (`12`) |
| C++23 modules not portable | Build complexity | Do not depend on modules (`14`) |
| Windows sandbox genuinely hard | Security gap | M1 ships the interface + fail-closed approvals; restricted-token enforcement lands M6. **Open question carried in `24`**: whether the loopback egress proxy works under AppContainer decides if Tier 2 is viable, or whether Tier 1 is the only honest Windows sandbox |
| MCP spec churn (stateless rewrite) | Rework | Isolate behind a `_meta` choke point; pin a revision (`07`) |
| Beast/Asio compile-time cost | Developer velocity | Isolate includes to network TUs (`14`) |
| Tool-count growth degrades accuracy | Quality | Hard tool budget + deferred loading |
| JIT default wrong for extension-heavy sessions | Performance | Re-measure with real extensions; per-extension opt-in exists |
| Solo/small-team velocity | Slower cadence | Ruthless scope; the core stays small by construction |

## Definition of done (every milestone)

1. Feature works end-to-end on all three platforms per the `24` CI matrix (full suite on Linux and Windows; macOS build + smoke unless the change is macOS-specific).
2. Smoke run recorded (transcript or artifact) demonstrating the changed path.
3. Eval suite run; no regression beyond noise.
4. Docs updated in the same change; no stale claims.
5. Cold start, binary size, and idle RSS within budget (`01`).
6. No new dependency without a measured justification against the size budget.

## Open questions

- Should the TUI move earlier than M7? Risk: building UI over an unstable event stream. Risk of late: no user feedback loop for seven months. Current bet: headless `exec` is usable enough to dogfood.
- Is the M7 Lua dogfood the right forcing function, or is it performative? If the Lua extensions are worse than a C++ equivalent would have been, that is the finding — and it means the API is wrong.
- When does SQLite FTS5 earn its place (`09`), and does it arrive before or after the memory subsystem?
- Is the untrusted extension tier ever built, or does trusted-only hold indefinitely?

## Sources

- https://arxiv.org/abs/2609.17394 — SWE-bench saturation; report per-instance outcomes, cost, and scaffold version
- https://www.swebench.com/ — mini-SWE-agent as the reproducible baseline to beat
- https://openai.com/index/building-codex-windows-sandbox/ — Windows sandbox required a separate elevated binary and dedicated users
- https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/changelog.mdx — MCP stateless rewrite
- https://en.cppreference.com/w/cpp/compiler_support/23 — C++23 feature support across compilers
- https://luajit.org/status.html — rolling releases, commit pinning
- https://luajit.org/faq.html — sandboxing stance
- https://github.com/LuaJIT/LuaJIT/issues/1092 — 3.0 status and bus-factor context
- https://maki.sh/docs/plugins/ — Lua extension precedent (dogfooding model)
- Docs `04`, `05`, `06`, `11`, `12`, `13`, `14`, `17`, `18`, `19`, `20` (each carries its own primary sources)
