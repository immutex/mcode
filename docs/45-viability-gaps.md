# Viability gaps: what other harnesses prove that mcode lacks

> TL;DR: The strongest measured lever is not a new surface. It is putting a check inside the edit loop: SWE-agent's own ablation scores lint-gated editing at 18.0% on SWE-bench Lite against 15.0% without, and mcode's verification gate fires once at the end from a command the user must arm. Second is a clean-context reviewer, which Devin measures at roughly 2 bugs per PR. Everything else in this document is ranked below those two, and four popular ideas (an LSP client, a repo map, cloud surfaces, a second extension ABI) are argued against with evidence.

## Method

Three research passes, one per harness group: [CC] and its docs; Codex CLI and opencode; Pi, Cline, Aider, Amp, Crush, Gemini CLI, Goose, OpenHands and the 2026 field. Each returned a ranked list with a URL per claim. The internal half is the repo's own measured record (`41`), the event and tool surfaces read from source, and one number from `mcode eval`.

Every item carries an evidence grade, because the grades are not equal:

| Grade | Meaning |
|---|---|
| `proven-number` | A controlled ablation, benchmark, or measured production delta with a figure |
| `proven-incident` | A named postmortem, CVE, or filed data-loss report |
| `widely-adopted` | Shipped by several harnesses, no measured delta found |
| `vendor-claim` | Vendor-published only, no independent replication |

Ranking is (evidence strength x how often a real project hits it). Cost is a rough class, not an estimate.

## The strongest measured lever: a check inside the edit loop

SWE-agent's SWE-bench Lite ablation: lint-gated editing **18.0%** resolved, the edit action without linting **15.0%**, no edit tool at all **10.3%**. The guardrail alone is +3.0pp. Across 2,294 trajectories, a median of 3 edits per trajectory raise a lint error, and an edit attempt has a 90.5% chance of eventually succeeding, so the recovery channel is load-bearing rather than cosmetic.

Crush implements the same shape with an LSP: send `didChange`, wait up to 5s for diagnostics to settle, append file and project diagnostics to the edit result, top 10 each. opencode appends severity-1 errors only, capped at 20 per file. SWE-agent runs flake8, and **reverts the edit** when it prints anything.

Cursor measured the adjacent effect: agents repeatedly retried the same failing command until the harness started naming the constraint responsible, after which offline eval improved significantly and sandboxed agents stopped **40% less often**.

mcode's position today: `--verify <command>` arms a gate that runs **once, at the end**, and `verify.command` is the only alternative. Nothing runs between an edit and the next model turn. `mcode eval` on this machine reports `false_success_rate: 0.9750` — 78 of 81 recorded runs had no verification armed at all. The "trust" bet is the least exercised part of the product.

**Decision: run a per-edit checker, not a language server.** opencode ships 30+ LSP servers and disables them by default, telling users verbatim that "in many projects it is better to have the agent run lint, typecheck, or other diagnostic CLI tools directly", and its v2 built-in tool set no longer contains an `lsp` tool. A per-project command list (Aider's `--lint-cmd` shape) captures most of the value with none of the server lifecycle. The revert-on-failure behaviour is the part worth copying exactly: it must happen inside the `edit` tool, before any result reaches the model.

## Ranked capability gaps

| # | Gap | Evidence | Cost | mcode today |
|---|---|---|---|---|
| 1 | Diagnostics or lint appended to the edit result, with revert | `proven-number` | small | Nothing between an edit and the next turn |
| 2 | Clean-context reviewer subagent | `proven-number` | small | Read-only fan-out exists; no reviewer role |
| 3 | Hooks that fire for subagent tool calls, and a wider event set | `proven-incident` | medium | 17 kinds, 3 vetoable, subagent calls unobserved |
| 4 | Sandbox denial reported in the tool result | `proven-number` | small | Denies at the OS layer, names no host or path |
| 5 | Plan mode as a runtime mode with an approval transition | `proven-number` | small | Deny table exists; no switch, no approval |
| 6 | Checkpoint restore that conflict-checks before writing | `proven-incident` | large | `/undo` and `/rewind` exist; no conflict check |
| 7 | Search subagent returning `file:line` spans | `proven-number` | medium | `grep` plus `task`, no span contract |
| 8 | ACP server mode plus a registry entry | `proven-number` | medium | Declared in `16`; not built |
| 9 | Cost attribution by model, call site, subagent, MCP server | `widely-adopted` | medium | `Usage` carries totals only |
| 10 | Workspace trust that enumerates what a clone contributes | `widely-adopted` | medium | Extension trees gated; instructions, skills, MCP, rules load |
| 11 | Per-command ephemeral network allowlist | `widely-adopted` | medium | Grants persist per domain per project |
| 12 | Fail closed when the sandbox is unavailable; protected paths | `widely-adopted` | small | Downgrades to the permission engine and continues |
| 13 | Model fallback chain | `widely-adopted` | medium | A retired pinned model ends the session |
| 14 | Skill portability: `.agents/skills` and an install command | `proven-number` | small | Reads `.claude/skills`; no `.agents`, no install |
| 15 | Oracle: a stronger model as a read-only second opinion | `proven-number` | small | Tiering exists; no oracle call site |
| 16 | MCP 2026-07-28 stateless revision and MRTR | `proven-number` | medium | Pinned revision, `_meta` choke point ready |
| 17 | Worktree-isolated parallel writers | `proven-incident` | large | `--worktree` exists; no isolation enforcement |
| 18 | Durable sessions with a supervisor | `proven-number` | large | JSONL plus `--resume`; dies with the terminal |
| 19 | TLS-inspecting egress with credential masking | `proven-number` | large | Decides on the client-supplied hostname |
| 20 | OTel export | `widely-adopted` | large | Decision 34: none, and never by default |
| 21 | A CI entry point (GitHub Action) | `widely-adopted` | small | `exec --json` exists; no action, no OIDC auth |
| 22 | Unix socket deny by default inside the sandbox | `widely-adopted` | medium | Convention only |

The four worth explaining:

**2. Clean-context reviewer.** Devin Review catches an average of 2 bugs per PR, roughly 58% severe, on PRs Devin itself wrote. Cognition attributes the gain to clean context rather than to a second opinion: the reviewer "gets to skip this extraneous context, only look at the diff". mcode already has the machinery, and the constraint is negative: the writer's trace must never reach the reviewer, enforced in the core rather than by convention.

**3. Hooks must fire for subagents.** This is a `proven-incident` with data loss attached. [CC] issue 84701: an unattended background subagent ran `rm -rf` on a home directory for about 5.5 minutes and destroyed real project data, and the reporter's hook guard did not fire for subagent-issued Bash calls. Issue 88441 reports the same for `Task`-spawned subagents. mcode's `tool_pre_call` veto is the right mechanism and its coverage is the gap: nothing subscribes to a child loop's dispatches. [CC] exposes 35 events and Codex 12; mcode has 17 and three vetoable. The events mcode lacks that matter most are a permission request, pre/post compaction, and subagent start/stop.

**6. Checkpoint conflict-checking.** Two filed incidents give the rule. opencode #53560: `/undo` rewrote 227 files (46,743 removed lines) and `/restore` rewrote 15,232 files across a monorepo, destroying uncommitted work from other tools, on a session with zero recorded patches. Cline #15023: restoring a session permanently deleted a `.gitignore`d folder of source assets, bypassing the Recycle Bin with no VSS copy. Having an old copy is not permission to overwrite current contents.

mcode is half-guarded, and the guarded half is the wrong half. `snapshot_restore.cxx` refuses a snapshot index entry that is absolute or contains `..`, so a tampered index cannot point a restore outside the workspace. Nothing compares the file's current bytes against what the snapshot recorded, so a restore overwrites whatever is there now. The missing check is the content one: refuse, or report a conflict, when the target changed since the snapshot was taken. The second axis is also missing, since [CC]'s rewind menu offers files only, conversation only, or both, and mcode's `/undo` and `/rewind` move both together.

**17. Worktree-isolated writers contradicts a stated non-goal.** `01` says "Not a multi-agent swarm. One writer. Read-only fan-out only." The evidence is that parallelism without isolation is worse than none: Anthropic's 16-agent compiler run had "every agent hit the same bug, fix that bug, and then overwrite each other's changes", and Cursor's first attempt without isolation slowed 20 agents to "the effective throughput of two or three". The safe first step is **best-of-N**, which needs no merge contract because the outputs are alternatives rather than fragments, and Cursor documents that it deliberately does not merge back. This is a decision to make explicitly, not a feature to slip in.

## Ranked quality-of-life gaps

| # | Gap | Evidence | Cost |
|---|---|---|---|
| 1 | Completion and approval notifications with a method ladder | `proven-incident` | small |
| 2 | `terminal-setup` and modified-Enter multiline | `proven-incident` | small |
| 3 | Clipboard: `/copy`, block picker, OSC 52 with a file fallback | `widely-adopted` | small |
| 4 | Image and screenshot paste into the prompt | `widely-adopted` | small |
| 5 | Background job list with attach, wait, completion events | `widely-adopted` | medium |
| 6 | Vim mode and a user keybindings file | `widely-adopted` | small |
| 7 | Custom status-line command fed session JSON on stdin | `widely-adopted` | small |
| 8 | `Ctrl+G` external editor for long prompts and plan review | `widely-adopted` | small |
| 9 | Prompt queue and stash | `widely-adopted` | small |
| 10 | In-session config and policy inspection (`/hooks`, `/permissions`) | `widely-adopted` | small |
| 11 | Shell completions | `widely-adopted` | small |
| 12 | `!`-prefixed shell escape in the composer | `widely-adopted` | small |
| 13 | Session naming, archive, grouping | `widely-adopted` | small |
| 14 | Screen-reader mode | `widely-adopted` | small to medium |

Three carry a specific lesson.

**1. The method ladder, not the escape sequence.** mcode already emits OSC 9 with a BEL fallback (`src/mcode/tui/notify.cxx`) on two triggers: approval needed and turn complete. What it lacks is the ladder and the policy. Gemini CLI sends OSC 9 with a BEL fallback; Codex exposes `notification_method = auto|osc9|bel` plus `notification_condition = unfocused|always` and a top-level `notify` command that receives a JSON payload; Crush has `notifications auto|native|osc|bell|disabled`; [CC] sends a desktop notification only in iTerm2, Ghostty and Kitty and rings the bell elsewhere, with a documented 6-second idle gate before a permission prompt notifies. The demand is filed and large (Gemini CLI issue 4310, 79 reactions; opencode issue 213, 30 +1), and two of three vendor requests were closed as duplicate or not-planned, so it is wanted but under-served. The gap is the native desktop path, a configurable channel and condition, and the trigger set.

**2. Modified Enter needs a config writer, not a probe.** [CC] accepts Shift+Enter natively in seven terminals and `/terminal-setup` writes the keybinding into the ones that need it, backing up and merging the config. The complaints are the most-reacted issues found (issue 1282, 90 reactions; issue 1758, 65). mcode's `\`+Enter is a defensible default because ConPTY does not reliably carry the Kitty protocol, but the common terminals are left unserved and a config writer is the documented fix.

**3. The clipboard ladder, because a sandboxed `pbcopy` fails.** [CC] writes from the CLI process rather than from a sandboxed command, sends OSC 52 outside tmux, and always writes what it copied to a file and prints the path. iTerm2 ignores OSC 52 until the user enables it and macOS Terminal.app does not support it, so the file fallback is not optional.

## Already planned, and the evidence for their rank

`44` Wave 2 and 3 already specify LSP (L), background jobs (B), image input (M), OSC 52 (C) and ACP (A). They are listed above with their evidence rather than presented as discoveries. Two notes on that:

- **LSP should drop to the bottom of Wave 2, or out of the core.** The evidence is against a core client: opencode ships 30+ servers and disables them by default, its v2 tool set dropped the `lsp` tool, and the measured win belongs to the cheaper lint gate. If it is built, it belongs as an optional Lua extension with a per-language server table, on top of gap 1.
- **Background jobs and durable sessions are one subsystem.** [CC] separates them explicitly ("a background bash command runs one shell command without blocking the conversation; it doesn't spawn an agent"), but both need a supervisor that outlives the TUI and a job registry. Building them twice is the trap.

## What mcode already matches

Checked against both Codex and opencode, which are the closest in surface: structured compaction with a summary, hierarchical `AGENTS.md` with closest-wins, skills with on-demand loading, MCP over stdio and Streamable HTTP with tools, resources and prompts, OS sandboxing on all three platforms, streaming markdown with a cell diff and a command palette, and read-only subagent fan-out. The deltas are individual commands and policy language, not architecture.

Two of mcode's decisions are confirmed rather than challenged:

- **No repo map in the prompt (decision 33).** Aider credits its repo map with selecting the right file in 70.3% of benchmark tasks but publishes no `--map-tokens 0` ablation anywhere. What is published is the cost: issue #5529 reports the Linux kernel taking about 20 minutes per scan and, in 0.86.2, re-running it every request, making Aider "unusable"; issue #3297 reports a 487-file repo freezing at 15%. Cursor's index that does pay is for grep speed, not prompt injection. If mcode ever builds an index, build that one.
- **No telemetry by default (decision 34).** Defensible for a solo developer. It is also the one place mcode diverges from five of six surveyed harnesses, and the cost is concrete: [CC] states OTel export is the only option that streams per-user token and cost metrics into your own stack on Bedrock, Vertex and Foundry. Gap 20 is a real team-adoption blocker, not a nicety.

## What the evidence says not to build

| Not built | Why |
|---|---|
| An LSP client in the core | The vendor with the most LSP support disables it by default and dropped the tool in v2. Gap 1 is the same mechanism at a fraction of the cost |
| A repo map injected into the prompt | No published ablation; documented catastrophic cost on large repos; decision 33 already right |
| A full HTTP/OpenAPI server | opencode's client/server split is load-bearing for exactly four things: multi-client, reattach, remote edit, warm start. A minimal loopback session daemon captures all four; OpenAPI generation, mDNS and CORS do not |
| A second extension ABI | [CC]'s mods API is an in-process JS hook layer with UI rendering. Luau already is that, with a capability boundary |
| Cloud, web, mobile, Slack, computer use | Hosted product surfaces. mcode competes on the local loop |

## Design decisions this would change

1. **`01` §Non-goals: "One writer. Read-only fan-out only."** Gap 17 needs a decision. The evidence supports best-of-N as the first step and does not support unconstrained parallel writers.
2. **`01` §Non-goals: "Not an IDE. Editor integration via protocol, not a GUI."** Gap 8 is consistent with this, and the shape is smaller than it sounds: [CC]'s own editor surface is a GUI shell over the CLI, talking to it through a local MCP server named `ide` on loopback with a token in a lock file. mcode has an MCP **client** and no server role, which is the concrete missing piece.
3. **Decision 34, no telemetry.** Gap 20 is the only item in this document that is purely about team adoption. It should be re-decided rather than inherited.

## Open questions

- Does a per-edit lint gate pay off at mcode's scale, or only on benchmark-shaped tasks? The SWE-agent number is one ablation on one benchmark.
- Is `false_success_rate` measuring the right thing? 0.975 is mostly this developer never arming `--verify`, which may say more about the ergonomics of arming it than about the gate.
- Gap 3 (hooks for subagents) is cheap and closes a data-loss path. Why is it not already in `44`?
- Which of gaps 9, 10, 16 and 20 are actually blocking a real user today, as opposed to plausible? No evidence of a filed mcode issue was found for any of them, because there are no users yet.

## Sources

Retrieved 2026-10-13.

- SWE-agent linting ablation: https://arxiv.org/html/2405.15793v3
- Crush edit tool and LSP client: https://raw.githubusercontent.com/charmbracelet/crush/main/internal/agent/tools/edit.go, https://raw.githubusercontent.com/charmbracelet/crush/main/internal/lsp/client.go
- opencode LSP policy, server, agents, permissions, config, skills: https://opencode.ai/docs/lsp, https://opencode.ai/docs/server, https://opencode.ai/docs/agents, https://opencode.ai/docs/permissions, https://opencode.ai/docs/config, https://opencode.ai/docs/skills
- opencode revert incidents: https://github.com/anomalyco/opencode/issues/53560
- Cline checkpoint data loss: https://github.com/cline/cline/issues/15023, https://docs.cline.bot/core-workflows/checkpoints
- Codex hooks, auto-review, sandboxing, worktrees, non-interactive mode, GitHub Action, config layers, AGENTS.md, skills: https://learn.chatgpt.com/docs/hooks, https://learn.chatgpt.com/docs/sandboxing/auto-review, https://learn.chatgpt.com/docs/agent-approvals-security, https://learn.chatgpt.com/docs/environments/git-worktrees, https://learn.chatgpt.com/docs/non-interactive-mode, https://learn.chatgpt.com/docs/github-action, https://learn.chatgpt.com/docs/config-file/config-advanced, https://learn.chatgpt.com/docs/agent-configuration/agents-md, https://learn.chatgpt.com/docs/build-skills
- Codex internals: https://github.com/openai/codex/tree/main/codex-rs/thread-store, https://github.com/openai/codex/tree/main/codex-rs/core/src/unified_exec
- Cognition SWE-grep and Devin Review: https://cognition.com/blog/swe-grep, https://cognition.com/blog/multi-agents-working
- Cursor sandboxing and worktrees: https://cursor.com/blog/agent-sandboxing, https://cursor.com/docs/configuration/worktrees.md, https://cursor.com/blog/fast-regex-search
- Amp oracle and model dial: https://ampcode.com/news/oracle, https://ampcode.com/docs/markdown/models-and-subagents
- Gemini CLI trusted folders, telemetry, notifications, plan mode: https://raw.githubusercontent.com/google-gemini/gemini-cli/main/docs/cli/trusted-folders.md, https://raw.githubusercontent.com/google-gemini/gemini-cli/main/docs/cli/telemetry.md, https://raw.githubusercontent.com/google-gemini/gemini-cli/main/docs/cli/notifications.md, https://raw.githubusercontent.com/google-gemini/gemini-cli/main/docs/cli/plan-mode.md
- Anthropic Sandbox Runtime: https://raw.githubusercontent.com/anthropics/sandbox-runtime/main/README.md
- Aider repo map and incidents: https://aider.chat/docs/repomap.html, https://github.com/Aider-AI/aider/issues/5529, https://github.com/Aider-AI/aider/issues/3297
- Agent Client Protocol and registry: https://agentclientprotocol.com/protocol/v1/overview, https://github.com/agentclientprotocol/registry
- MCP 2026-07-28 changelog and MRTR: https://raw.githubusercontent.com/modelcontextprotocol/modelcontextprotocol/main/docs/specification/2026-07-28/changelog.mdx
- Skills distribution: https://www.skills.sh/about, https://agentskills.io/clients.md
- OpenHands automations and metrics: https://docs.openhands.dev/openhands/usage/automations/overview.md, https://docs.openhands.dev/sdk/guides/metrics.md
- [CC] documentation: the vendor docs host, pages `hooks`, `hooks-guide`, `permissions`, `permission-modes`, `settings`, `sandboxing`, `memory`, `checkpointing`, `interactive-mode`, `terminal-config`, `statusline`, `sub-agents`, `agent-teams`, `workflows`, `monitoring-usage`, `github-actions`, `code-review`, `vs-code`, `jetbrains`, `plugins/code-intelligence`
- Internal: `docs/41-session-analysis.md`, `docs/44-0.1.0-batch.md`, `src/mcode/events/bus.hxx`, `src/mcode/tools/`, `mcode eval`
