# Prior Art: Coding-Agent Harness Architectures

> TL;DR: Every serious harness converged on the same skeleton — a single LLM loop over a small set of typed file/shell tools, sandboxed execution, file-based instruction loading, MCP for extension, and subagents for context isolation — and the differentiators are context management, permissions UX, and TUI quality.

## State of the field

All facts below verified against the cited repo/docs page unless marked `[UNVERIFIED]`.

| System | Lang/Runtime | Loop shape | Tool surface | Context strategy | Extension | Subagents | Permissions | UI | License |
|---|---|---|---|---|---|---|---|---|---|
| **Claude Code** | TS/Node 18+, native installer | Agentic loop w/ nested subagent loop; hook events at every stage (PreToolUse, PostToolUse, PreCompact, SubagentStart/Stop…) | Typed tools (Read/Edit/Write/Bash/…) + MCP tools; hooks can veto any tool call | CLAUDE.md + `.claude/rules/*.md` lazy-loaded (InstructionsLoaded hook); compaction w/ Pre/PostCompact hooks | Skills, plugins (marketplace), hooks (shell/HTTP/MCT/prompt/subagent), MCP | First-class (SubagentStart/Stop hooks, agent teams, teammate idle) | PermissionRequest hook + settings precedence (managed→CLI→local→project→user); allowManagedHooksOnly | Terminal + IDE + Desktop + web; headless | Proprietary (source-visible dist) `[UNVERIFIED]` |
| **Codex CLI (OpenAI)** | Rust (`codex-rs`), Apache-2.0 | Single loop; `approval_policy` gates each exec | Typed tools + shell; `sandbox_mode: read-only\|workspace-write\|danger-full-access`; Windows native sandbox (restricted tokens + synthetic SID + dedicated sandbox users + firewall rules; setup in separate elevated binary, runner in `codex-command-runner.exe`) | AGENTS.md; `model_auto_compact_token_limit` triggers auto history compaction; SQLite-backed resumable state | MCP (`mcp_elicitations` approval), skills (skill_approval), lifecycle hooks, profiles | Reviewer subagent (`approvals_reviewer: auto_review`), `/review` w/ separate `review_model` | OS sandbox (Seatbelt/macOS, Landlock+seccomp/Linux, custom Windows) + granular approval policy | TUI + IDE + desktop + `codex exec` headless | Apache-2.0 |
| **Pi (badlogic/pi-mono)** | TS/Node ≥22.19, MIT | Minimal agent runtime (`pi-agent-core`: tool calling + state mgmt) | Built-in tools + `!` shell commands; deliberately **no built-in permission system** | Prompt templates, skills, AGENTS.md; "ask the agent to explain itself" | Self-extensible: skills, extensions, themes, npm packages; RPC/print/JSON modes | Via extensions `[UNVERIFIED]` | None built-in — containerize (Docker, micro-VM via Gondolin, OpenShell) | `pi-tui`: differential-rendering terminal UI lib; standalone binaries | MIT |
| **opencode (sst)** | TS (Bun/npm), client+server | Build/plan agent split; Tab toggles | Typed tools; bash gated in plan mode | Files + shared session store | MCP, plugins, custom agents | `general` subagent (@general) | build=full access, plan=read-only+ask | TUI + desktop app (BETA) | `[UNVERIFIED]` |
| **Charm Crush** | Go (Charm ecosystem) | Session-based loop; model switch mid-session w/ context preserved | Typed tools + LSP-enhanced context; MCP (http/stdio/sse) | `context_paths` config; LSP symbols | MCP + `crushrc` (Bash-with-builtins config, incl. `permissions allow view edit`, 1Password secret fetch) | Not central `[UNVERIFIED]` | `permissions allow view edit` in crushrc | Bubbletea TUI, cross-platform incl. BSD/Android | `[UNVERIFIED]` |
| **Block Goose** | Rust, Apache-2.0 | Desktop+CLI+API agent loop | Built-in tools + 70+ MCP extensions; ACP providers | Recipes/distros: preconfigured providers+extensions | MCP; custom distributions (preconfigured builds) | `[UNVERIFIED]` | `[UNVERIFIED]` | Desktop app + CLI | Apache-2.0 (AAIF/Linux Foundation) |
| **OpenHands (Agent Canvas)** | TS frontend + Python agent-server SDK | Agent Server REST API runs conversations; backend-swappable | Docker sandbox by default; any ACP agent (Claude Code, Codex, Gemini) | Workspaces/events model in SDK | ACP + MCP; automations (cron/webhooks → Slack/GitHub/Linear) | Multi-backend orchestration | Sandbox-first (Docker/VM/cloud); "full access" warned against | Web control center, self-hosted | `[UNVERIFIED]` (multi-repo) |
| **SWE-agent / mini-SWE-agent** | Python, MIT | mini: ~100-line linear loop, **no tool-calling API** — model emits bash, executed via `subprocess.run` per action (stateless, sandbox-swappable) | Bash only | Completely linear history = trajectory = messages (FT/RL-friendly) | YAML config; history processors (full SWE-agent) | None | Sandbox = swap `subprocess.run`→`docker exec` | CLI + trajectory browser | MIT |
| **Aider** | Python | Chat loop w/ auto git commit per edit; watch-mode IDE comments | Edit formats (diff/whole-file), lint+test auto-fix loop | **Repo map**: tree-sitter ranked symbol graph of whole codebase | Git-native; config files | None | Git undo as safety net | CLI + IDE watch mode | `[UNVERIFIED]` |
| **Cline** | TS (VS Code ext, CLI, Tauri desktop, SDK) | Plan/Act mode toggle; approve-every-edit loop; checkpoints | Typed tools + bash w/ live output streaming; MCP + SDK plugins (`createTool`) | `.clinerules` + skills; checkpoint undo | SDK plugins w/ lifecycle hooks, MCP, multi-agent teams | Coordinator→specialist teams, persistent team state | Human-in-the-loop per edit/command; auto-approve toggle | IDE diffs, desktop, headless JSON CLI | Apache-2.0 |
| **Gemini CLI** | TS/Node, Apache-2.0 | Loop w/ built-in Google Search grounding | Built-in tools (file ops, shell, web fetch, search grounding) + MCP | GEMINI.md; `/compress` summary; conversation checkpointing | MCP; extensions; GitHub Action | `[UNVERIFIED]` | `[UNVERIFIED]` | TUI, `--output-format json/stream-json` | Apache-2.0 |
| **Amp** | Proprietary (source-available) `[UNVERIFIED]` | Main agent + subagents-as-tools | Edits, shell, MCP, search | **Handoff→auto-compaction at ~90% window** (Neo, 2026-05: "Handoff is gone"); fresh context + summary | MCP, plugins API | Search, Task workers, Librarian, Read Thread, **Oracle** (stronger model for review/debug); A2A threads w/ own context+workspace | `[UNVERIFIED]` | Editor-first (VS Code), threads | Proprietary `[UNVERIFIED]` |
| **Continue** | TS (VS Code, JetBrains, CLI) | Agent loop, final 2.0.0 | Typed tools + MCP | Config-as-code (YAML blocks in markdown) `[UNVERIFIED]` | MCP, blocks | `[UNVERIFIED]` | `[UNVERIFIED]` | IDE-first | Apache-2.0 — **repo archived, read-only** |
| **Codebuff → Freebuff** | TS monorepo (Bun) | **Specialized agent pipeline**: file-finder agents map codebase → implement → review agents | Tools + browser agents + parallel isolated workspaces | File-finding agents pre-map relevant files | `@codebuff/sdk` custom agents | Core design: multi-agent orchestration, desktop runs parallel agents in separate workspaces | `[UNVERIFIED]` | Desktop/CLI/Web/Cloud | `[UNVERIFIED]` (ad-supported free product) |
| **smolagents (HF)** | Python, Apache-2.0 | ReAct loop where **actions are Python code**, not JSON tool calls | Tools as Python funcs; MCP `ToolCollection`, LangChain, Hub tools | Linear memory | Hub share/pull of tools+agents | Multi-agent hierarchies | E2B/Docker/Modal sandboxes; `LocalPythonExecutor` explicitly NOT a security boundary | CLI (`smolagent`) | Apache-2.0 |
| **Open Interpreter (new)** | **Rust fork of Codex** | `/harness` switchable harness emulation (claude-code, kimi-code, zcode, swe-agent, minimal…) | Codex exec protocol-compatible; MCP, skills, hooks, permissions | Shared `AGENTS.md` + `.agents/skills` dirs (cross-agent portability) | ACP agent (`interpreter acp`), Codex SDK drop-in (`codexPathOverride`) | Via harness | Native sandboxing macOS/Linux/Windows | TUI, web chat | Apache-2.0 |

Key evidence points:
- SWE-agent's own team concluded custom tool interfaces were over-engineering: mini-swe-agent (bash-only, linear history, stateless `subprocess.run`) scores **>74% SWE-bench verified** and "beats [CC] and Codex on DeepSWE" (mini-swe-agent README). SWE-agent 1.0 + the model was SoTA on SWE-bench verified (Feb 2025); mini hit **65% in 100 lines of Python** (July 2025 announcement).
- **No mainstream harness embeds a scripting language — but Maki already proves the model.** [CC], Codex, opencode, Pi, and Goose are all JS/TS-on-a-runtime: their extension language *is* their runtime. **Maki** (Rust core + **Luau**, MIT, ~1.1k stars) ships its built-in tools as bundled plugins. Verified directly against the repo: `plugins/` contains **21 tool plugins** plus a shared `lib/` — `bash`, `read`, `write`, `edit`, `glob`, `grep`, `list`, `index`, `batch`, `view_image`, `code_execution`, `completion`, `question`, `task`, `todo_write`, `memory`, `skill`, `sessions`, `thinking`, `webfetch`, `websearch`. The Rust core exposes primitives only (`maki.fs.*`, `maki.net.request`, `maki.agent.session`) with policy in Lua; its source comments say so: "Rust exposes primitives only." It reports **70% pass at $2.06/pass** on its harness benchmark `[VENDOR]`. **imp** (`mlua`) does the same more narrowly.
  
  Two details worth noting. Maki ships a **`completion` plugin**, where mcode deliberately has no completion tool (`06`) — a genuine design divergence, not an oversight. And it makes `question` (the `ask_user` equivalent) a plugin, where mcode keeps `ask_user` core; the reasoning is in `06` §Core tool set (its absence makes the agent silently guess rather than ask).
  
  **Maki's runtime is Luau, and so is ours** (`27`). Luau is Roblox's Lua 5.1 derivative with its own type system; it is not bytecode- or dialect-compatible with LuaJIT. The precedent is about the *architecture* — native core, scripting ecosystem, primitives in the host language, policy in script — and the language now matches too.
  
  This corrects an earlier claim in this doc: mcode is **not** the first native harness with a scripting ecosystem — Maki is, and it is the closest thing to a direct precedent. The novelty is the *implementation* (C++23 rather than Rust) and the scale target, not the architecture. That is a better position anyway: the model is validated at benchmark level, and the risk is execution rather than concept.
- smolagents cites **30% fewer steps / 30% fewer LLM calls** for code-as-action vs JSON tool calls (papers 2402.01030, 2411.01747) — their claim, cited from their README.
- **Pi's minimalism is surface area, not runtime.** Pi ships 8 built-in tools, no permission system, no plugin ABI — genuinely small. But it embeds Node ≥22.19 and runs TypeScript extensions in-process via `jiti`. A third-party PTY benchmark measures Pi at **590.7 ms time-to-first-frame and 144.4 MB PSS** versus a native Rust harness at **14.0 ms / 27.8 MB** `[THIRD-PARTY, competitor-measured]`. Pi's docs never claim startup or RSS numbers — the minimalism claim is architectural, not quantitative. Copy Pi's *shape* (loop, tools, hook surface, session format); replace its *substrate*.
- Pi's session format is worth studying: **JSONL trees** where every entry carries `id`/`parentId`, the active branch is the path to a leaf, and `/tree`, `/fork`, `/clone` manipulate that structure. System prompt and tool loadout are persisted as *replayable system messages* (`sections` patched by name, `toolsAdded`/`toolsRemoved`) rather than as separate prompt state — directly relevant to cache-aligned prompt construction (`05`).
- Pi's extension event surface is deep and ordered: `tool_call` can mutate input or **block**, `context`/`context_with_system` transform the transcript, `before_agent_start` rewrites system-prompt sections, and multiple handlers stack on the same hook. Slow handlers delay the stream because they are awaited in order. This is the design mcode's `mcode.on` mirrors (`18`).
- Amp moved from manual compaction → handoff (Oct 2025) → automatic compaction at ~90% window (Neo, May 2026); live docs still reference handoff — doc drift is real even at top shops.
- Codex's Windows sandbox blog (May 2026) documents why AppContainer/Windows Sandbox/MIC all failed their requirements; final design = write-restricted tokens + synthetic SID + two dedicated local users + firewall rules, with setup split into a separate elevated binary.
- Pi explicitly ships **no permission system** and tells you to containerize — the minority position; everyone else builds in-process permission gates.
- Open Interpreter rebuilt itself as a Rust fork of Codex whose headline feature is *emulating other harnesses* — evidence that harness prompt/tool scaffolding measurably changes model performance, especially for low-cost models.

## What works / what doesn't

**Works (with numbers where found):**
- Bash-centric minimal loops: mini-swe-agent >74% SWE-bench verified with ~100 LOC and zero custom tools.
- Typed file tools (read/edit/write) + bash, the Claude Code/Codex shape: dominant default across 13/16 systems surveyed.
- OS-level sandboxing (Seatbelt/Landlock/Windows restricted tokens) + approval policy as separate axes — Codex's `sandbox_mode` × `approval_policy` split is the cleanest model found.
- File-based instructions (AGENTS.md/CLAUDE.md/GEMINI.md) — now a de facto cross-agent standard; Open Interpreter treats shared `.agents/skills` as a portability requirement.
- MCP as universal extension bus: Goose (70+ extensions), Crush, Gemini CLI, Cline, Continue, smolagents all speak it.
- Subagents for context isolation: Amp's design (own context window, parent gets only the summary) and Claude Code's hook-instrumented subagent lifecycle both validate it.
- Compaction over summarization-on-overflow: Codex (`model_auto_compact_token_limit`) and Amp (~90% threshold) both automated it; Amp explicitly killed manual handoff.

**Doesn't:**
- Heavy custom tool/ACI interfaces (original SWE-agent): superseded by its own authors.
- Permission systems in-process without OS enforcement: Pi punted entirely; Codex's unelevated Windows sandbox "network suppression was advisory" and got replaced.
- JSON-tool-call-only loops: measurably more steps than code-actions on hard tasks (smolagents claim `[VENDOR]`).
- Framework sprawl: Continue archived; LangChain-style abstraction layers absent from every production harness listed.
- Human-approves-everything: Codex removed `untrusted` approval policy entirely; approval fatigue documented as a core design driver in their Windows sandbox post.

## Recommended design for mcode

1. **Loop**: single agent loop, no framework. Eight core tools plus Lua extensions and MCP tools flattened into one dispatch table (`06` §Core tool set is canonical). Bash stays stateless-per-call by default (mini-swe-agent pattern) with opt-in persistent shell sessions.
2. **Permissions = two orthogonal axes**, copying Codex: `sandbox` (OS-enforced: none/workspace/danger) × `approval` (never/on-request/always). On Windows use restricted tokens if elevated setup is acceptable; otherwise fail closed and require approval per networked command — do NOT ship advisory env-poisoning (Codex tried, replaced it).
3. **Context**: file-based instructions (`AGENTS.md`-compatible, closest-wins, lazy-loaded per-directory) plus an inspectable `MEMORY.md` index (`08`, `09`); auto-compaction at a configurable token threshold (default ~80%) with Pre/PostCompact hooks; compaction = structured summary + pinned facts, not lossy tail-drop.
4. **Subagents as tools**: spawn with isolated context, return summary + file list only; read-only scout subagent as the first specialization; no inter-subagent messaging in v1.
5. **Extension**: **Luau is the plugin ABI** (`17`–`20`) — tools, commands, hooks, and context contributions all register through one `mcode` API and land in the same registries as the built-ins. MCP (stdio + HTTP) is a *tool source*, not agent logic. Skills are markdown + optional scripts, loaded on demand. Hooks are Luau functions on the event bus, with veto limited to `Pre*` events. Project-scoped extensions are inert until a hash-pinned trust grant.
6. **UI**: immediate-mode TUI with differential rendering (pi-tui approach) in C++23; headless `mcode exec --json` mode from day one (Codex/Gemini CLI pattern); ACP compatibility later, not first.
7. **C++23 specifics**: single static binary, no runtime deps beyond TLS; one event loop with Asio for streaming and subprocess pipes; `std::expected` for tool errors; yyjson for dynamic JSON (tool schemas arrive at runtime); process sandbox behind a per-OS strategy interface.

## Traps

- **Building custom tool interfaces for everything** (SWE-agent 1.0 path): your own authors will deprecate it. Keep tool count minimal; let bash cover the tail.
- **In-process permission checks as a security boundary**: advisory only. Either get OS enforcement or label the mode "convenience, not security" in docs.
- **Calling a scripting VM a sandbox**: every project that tried it (Kong, Redis, Luanti) documents that it stops only accidents, and Luanti's CVE-2026-40959 was a LuaJIT mod-sandbox escape to RCE. A capability boundary is not an OS sandbox (`12`).
- **Embedding a scripting runtime without deciding its trust model first**: the trust decision drives the process model, which drives the API shape. Decide before writing the loader (`12`, `19`).
- **Doc drift between marketing and docs**: Amp ships contradictory handoff docs post-Neo. Keep one source of truth; generate docs from config schema.
- **Summarize-only context management**: repeated lossy compaction degrades long sessions (widely reported for Gemini CLI/Claude Code users; Gemini CLI added manual `/compress` + checkpointing as mitigations). Prefer structured compaction + file-persistence of decisions.
- **Approval fatigue**: per-command prompts train users to spam-approve (Codex removed `untrusted` for this reason). Default to sandboxed-auto + escalate-on-anomaly.
- **Windows as an afterthought**: Codex needed a dedicated sandbox binary + two local users to get real Windows isolation; plan the Windows sandbox strategy before the TUI polish.
- **Fork-adopting another harness's prompt scaffold without its context rules** (Open Interpreter's harness emulation cuts both ways): harness performance is coupled to model + prompt + tools as a unit.

## Open questions

- Do skills (progressive-disclosure markdown) measurably outperform plain AGENTS.md sections for tool-selection accuracy? No public ablation found. `[UNVERIFIED]`
- Optimal compaction threshold: Amp uses ~90%; Codex exposes it as config. No published ablation.
- Is ACP (Agent Client Protocol) going to become the standard editor↔agent interface alongside MCP, or stay a Goose/OpenHands/Open Interpreter niche? Watch it.
- Code-as-action (smolagents) vs JSON tool-calls for frontier models in 2026: the 30%-fewer-steps result predates native tool-calling improvements; needs re-validation.

## Sources

Fetched this session:
- https://raw.githubusercontent.com/anthropics/claude-code/main/README.md
- https://code.claude.com/docs/en/hooks.md (hook events, lifecycle, veto protocol)
- https://code.claude.com/docs/en/settings.md (settings precedence)
- https://raw.githubusercontent.com/openai/codex/main/README.md
- https://learn.chatgpt.com/docs/config-file/config-reference (sandbox_mode, approval_policy, compaction, reviewer subagent)
- https://openai.com/index/building-codex-windows-sandbox/ (Windows sandbox design)
- https://raw.githubusercontent.com/badlogic/pi-mono/main/README.md and /packages/coding-agent/README.md
- https://raw.githubusercontent.com/sst/opencode/dev/README.md
- https://raw.githubusercontent.com/charmbracelet/crush/main/README.md
- https://raw.githubusercontent.com/block/goose/main/README.md
- https://raw.githubusercontent.com/All-Hands-AI/OpenHands/main/README.md
- https://raw.githubusercontent.com/SWE-agent/SWE-agent/main/README.md
- https://raw.githubusercontent.com/SWE-agent/mini-swe-agent/main/README.md
- https://raw.githubusercontent.com/Aider-AI/aider/main/README.md
- https://raw.githubusercontent.com/cline/cline/main/README.md
- https://raw.githubusercontent.com/google-gemini/gemini-cli/main/README.md
- https://raw.githubusercontent.com/ampcode/amp/main/README.md (404 — repo not at that path; Amp facts from ampcode.com search results: /notes/how-to-build-an-agent, /docs/models-and-subagents, /news/oracle, /news/neo, /docs/orbs/agent-to-agent — snippets retrieved via search, full pages not fetched)
- https://raw.githubusercontent.com/continuedev/continue/main/README.md
- https://raw.githubusercontent.com/CodebuffAI/codebuff/main/README.md
- https://raw.githubusercontent.com/huggingface/smolagents/main/README.md
- https://raw.githubusercontent.com/openinterpreter/open-interpreter/main/README.md
- https://geminicli.com/docs/reference/commands/ (`/compress`) — via search snippet
