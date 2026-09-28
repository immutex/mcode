# Configuration & CLI

> TL;DR: One TOML config with four scopes and a **project scope that can only narrow** (never widen) privileges, plus a small consolidated CLI surface with documented exit codes and a versioned `exec --json` event stream.

## Config

One file, `config.toml`, merged from four scopes. **Higher scope wins on scalars; lists merge; capability-widening keys are honored only from user/managed scope.**

| Scope | Location | Can widen privileges? |
|---|---|---|
| managed | `<install>/config.toml` | yes (org policy) |
| user | `~/.config/mcode/config.toml` (`%APPDATA%\mcode\`) | yes |
| project | `<repo>/.mcode/config.toml` | **no — may only add `ask`/`deny`** |
| session | CLI flags, `--config key=value` | no |

The never-widen rule is the defense against a cloned repo shipping a config that grants itself permissions (`12` T3). It applies to the same keys as the permission engine, and it is enforced at merge time, not at use time.

Two load-time rules make the merge safe:

- **Unknown sections are refused.** A top-level key outside `model`, `agent`, `context`, `sandbox`, `ui`, `extensions` and `permissions` is a typo, and storing it silently means the user believes a setting took effect when it did not. Checked at section granularity; the nested keys belong to the docs that define them.
- **Project scope may only APPEND.** A project file may name `permissions.deny` or `permissions.ask` and nothing else, and only as a list. A scalar at one of those names — `permissions.deny = "everything"`, which is valid TOML — would otherwise overwrite the user's list in the merge, and every consumer reads the result through a string-array accessor that turns the type mismatch into an empty list. The user's deny rules would vanish and the blocked tool would become callable. The merge itself also refuses to let a non-list replace a list, so a future layer type inherits the rule rather than re-deriving it.

```toml
[model]
provider = "openai-compatible"     # openai-compatible | anthropic | gemini
base_url = "https://…"
model    = "…"
api_key_env = "MCODE_API_KEY"
tier.plan = "…"                    # frontier
tier.act  = "…"                    # workhorse (default)
tier.side = "…"                    # cheap: titles, compaction, search

[agent]
max_steps        = 100
max_tokens       = 2_000_000
max_budget_usd   = 5.0
compact_at_pct   = 80              # of usable window
clear_tools_at_pct = 60
max_parallel_tools = 8

[context]
read_lines_default = 100           # SWE-agent ablation: 100 beats 30 and full-file
tool_result_inline_tokens = 2000   # overflow spills to .mcode/artifacts/
instruction_chain_bytes = 32768    # per-file cap (`08`)
# Chain total is hardcoded at 64 KiB (`08`) — a safety limit, deliberately NOT configurable.

[sandbox]
mode     = "workspace"             # none | workspace | danger
approval = "on-request"            # never | on-request | always
egress_allowlist = []              # empty ⇒ prompt-and-add

[ui]
show_tool_calls = true
theme           = "auto"
verbosity       = "concise"

[extensions]
enabled = true                     # master switch for ALL extensions

# Per-extension control. Absent ⇒ the default from 23 (enabled when its
# capability is configured, disabled otherwise).
[extensions.web]
enabled = true
config = { search_provider = "exa" }   # extension-private; validated against its manifest

[extensions.git]
enabled = false

[extensions.github]
enabled = true
config = { default_remote = "origin" }

[telemetry]
enabled = false                    # never phones home; see "Telemetry" below
```

Precedence for instruction files (`08`) and permissions (`12`) is defined in those docs; this table defines precedence for *config*. They use the same four scopes so a user reasons about one model.

**Extension enablement follows the never-widen rule in reverse.** Disabling is narrowing, so any scope may disable any extension — a project may turn off an extension the user enabled. Enabling is *not* narrowing: a project config **cannot enable** an extension the user scope disabled.

One carve-out: `task` is load-bearing (`23`), so a project scope may disable it only with an explicit `allow_disable_task = true` in that same project config. Without it, the project's `task` setting is ignored with a warning. The point is that disabling delegation is a deliberate act, never a side effect of cloning a repo.

Validation is strict: unknown keys are an error (catches typos that would silently do nothing), and every value has a documented range. `mcode config check` validates and prints the merged result with per-key provenance.

## CLI surface

Consolidated. `mcode <command> [flags]`, and running with a bare prompt implies `run`.

| Command | Purpose |
|---|---|
| `mcode [prompt]` | Interactive session, or one-shot if a prompt is given |
| `mcode run <prompt>` | Explicit one-shot |
| `mcode exec --json` | Headless; emits the event stream on stdout (`20`) |
| `mcode resume <session-id>` | Replay and continue a session |
| `mcode sessions list\|show\|rm` | Session management |
| `mcode config check\|show` | Validate / print merged config with provenance |
| `mcode ext list\|doctor\|disable <name>` | Extension introspection (`19`) |
| `mcode skill list\|validate` | Skill discovery and linting (`08`) |
| `mcode memory show\|reindex\|gc` | Memory inspection (`09`) |
| `mcode prompt lint` | System-prompt interference check (`21`) |
| `mcode eval <suite>` | Run the fixture suite (`11`) |

Global flags: `--model`, `--max-budget-usd`, `--max-steps`, `--sandbox`, `--approval`, `--yolo`, `--no-sandbox`, `--no-extensions`, `--config key=value`, `--cwd`, `-v/--verbose`, `--version`.

Slash commands in-session: `/help`, `/clear`, `/compact`, `/context`, `/model`, `/tools`, `/skill`, `/mcp`, `/session`, `/reload`, `/exit`. Commands are registered in one registry, so Lua extensions add to the same surface (`18`) rather than a parallel one.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Run completed, verification gate passed |
| 1 | Run completed but the verification gate failed |
| 2 | Usage error (bad flags, unknown command, invalid config) |
| 3 | Budget exhausted (steps/tokens/USD) — partial state preserved, resumable |
| 4 | Provider error after retry exhaustion |
| 5 | Permission denied by policy (not by user decline) |
| 130 | Interrupted (SIGINT) — session is resumable |

### `exec --json`

The headless contract, versioned. One JSON object per line on stdout (`20`'s envelope), diagnostics on stderr. The stream starts with `{"v":1,"kind":"run.start"}` and ends with exactly one `run.end` carrying the exit code. Unknown kinds are skipped by consumers — additive forever. This is the interface editors and CI use, so it is specified and tested, not incidental.

## Telemetry

**None by default, and none in v1 at all.** mcode does not phone home, does not collect usage, and does not require an account. Local logs live under `.mcode/logs/` with rotation and a retention cap. If opt-in telemetry ever ships it must be: off by default, disclosed in one line at first run, inspectable (`mcode telemetry show`), and never carrying prompt or file content. Saying this plainly is cheaper than being asked.

## Concurrency

Two mcode processes on one workspace is **supported for read-only work and serialized for writes**:

- Session logs are per-session files; no contention.
- `.mcode/` carries a lock file (`lock`, containing pid + start time) taken on first write. A second writer waits or fails with a clear message; it never corrupts.
- The FTS5 index is opened by one writer at a time (SQLite WAL, `busy_timeout` set).
- Subagent worktrees live under `.mcode/wt/<run-id>/` so parallel writers never share a tree (`10`).

## Open questions

- Should `--config key=value` be allowed to widen privileges (it is session scope)? Leaning no, for consistency with the never-widen rule.
- Is a lock file enough, or do we need per-file advisory locks for concurrent writers? Measure once real users hit it.
- Does `exec --json` need a schema file (JSON Schema) checked into the repo so consumers can validate? Probably yes at M5.
- Config file format: TOML is chosen for comment support and no-significant-whitespace. Revisit only if a real schema need appears.

## Sources

- https://arxiv.org/html/2405.15793v3 — SWE-agent viewport ablation (100-line window)
- https://arxiv.org/html/2605.24660v1 — per-tool schema token cost
- https://developers.openai.com/api/docs/guides/structured-outputs — strict schema validation conventions
- https://neovim.io/doc/user/lua/ — `vim.secure` trust model, config scope precedents
- https://docs.konghq.com/gateway/latest/plugin-development/ — declarative config validation
- https://www.sqlite.org/wal.html — WAL concurrency semantics
- Docs `03` (persistence), `04` (budgets), `06` (tool caps), `08` (instruction caps), `11` (eval suite), `12` (permission scopes), `19` (extensions), `20` (event envelope), `21` (prompt lint)
