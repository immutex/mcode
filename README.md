<h1 align="center">mcode</h1>

<p align="center">
  <strong>A native C++23 coding-agent harness with a Luau extension layer.</strong><br>
  One static binary. No runtime. A scripting ecosystem that costs nothing until you use it.
</p>

<p align="center">
  <a href="https://github.com/immutex/mcode/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/immutex/mcode/actions/workflows/ci.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="License: Apache-2.0" src="https://img.shields.io/badge/License-Apache--2.0-blue.svg?style=flat-square"></a>
  <img alt="Platforms: Windows, Linux, macOS" src="https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg?style=flat-square">
  <img alt="C++23" src="https://img.shields.io/badge/C%2B%2B-23-00599C.svg?style=flat-square&logo=cplusplus&logoColor=white">
  <img alt="Binary: 9.1 MB" src="https://img.shields.io/badge/binary-9.1%20MB-success.svg?style=flat-square">
  <img alt="Runtime dependencies: 9" src="https://img.shields.io/badge/runtime%20deps-9-success.svg?style=flat-square">
</p>

## What mcode is

mcode is a coding agent you run from a terminal. It reads and edits your files,
runs your commands, and asks before it does anything you have not allowed.

Every serious harness converged on the same skeleton: one model loop over a small
set of typed file and shell tools, OS-level sandboxing, markdown instruction
files, MCP for reach. mcode uses that skeleton and competes on four things the
field leaves alone.

|  | mcode | The field |
|---|---|---|
| Runtime | None, just a static binary | Node, Python, Go, or Rust |
| Extension layer | Luau VM in-process, ~110 µs marginal per extension | The runtime *is* the extension language |
| Idle footprint | ≤ 30 MB budget | 100 MB+ is normal |
| Cold start | ≤ 15 ms budget, built for per-invocation spawning | Seconds; designed as a long-lived session |
| TUI | Custom ANSI cell-diff renderer, no TUI library | Blessed framework, or plain stdout |
| Dependencies | 9 runtime, size is the binding constraint | Whatever the ecosystem drags in |
| Extension ABI | A scripting language: versionable, writable without a compiler | A C ABI, `dlopen`, or nothing |

Every comparable harness ships a runtime: Node, Python, Go, or Rust. A
statically linked C++23 binary cold-starts in single-digit milliseconds with a
small resident set, which is what makes an agent composable. Scriptable,
pipeable, spawnable in parallel, embeddable in CI. A native Rust harness has been
measured at 14.0 ms to first frame and 27.8 MB resident; a TypeScript one at
590.7 ms and 144.4 MB (`[THIRD-PARTY]`,
[`docs/02`](docs/02-prior-art.md)). Nobody in this space currently optimizes for
it.

No mainstream harness embeds a scripting language, because their extension
language *is* their runtime. mcode embeds Luau, so the extension ecosystem costs
under 1 MB, costs nothing when unused, and extensions are written without a
compiler.

Models degrade before their context window fills, and prefix instability destroys
prompt-cache economics. mcode treats context as a budget with explicit numbers
rather than a buffer you append to.

Top-tier resolve rates are saturated and statistically hard to separate. What is
left is whether the agent can be trusted not to game the grader, silently shrink
scope, or claim success it never verified.

Tool names, loop shape, permission axes and TUI idioms are deliberately copied
from systems that already validated them.

Our philosophy:

```text
native core, scripting ecosystem
context is a budget, not a buffer
enforce invariants in the harness, not the prompt
fail closed on authority, fail open on capability
boring technology, spent novelty budget
measure or it did not happen
```

> [!NOTE]
> **C++ provides the runtime primitives. Lua provides the ecosystem.** That single
> rule is why the core knows four abstractions (`ModelClient`, `Context`,
> `ToolRegistry`, `EventBus`) and nothing about Lua, MCP, skills, or the
> terminal. New capability attaches to a registry or the event bus, never to the
> loop.

## Quick start

One command downloads the binary for your platform, verifies its SHA-256, puts it
on your PATH, and starts a setup wizard. No compiler, no Python, no Conan.

**Windows** (PowerShell)

```powershell
irm https://raw.githubusercontent.com/immutex/mcode/master/install.ps1 | iex
```

**Linux / macOS**

```bash
curl -fsSL https://raw.githubusercontent.com/immutex/mcode/master/install.sh | sh
```

The wizard picks a provider, takes an API key, and verifies the endpoint with a
real turn before it writes anything. It writes exactly one file: `mcode config`'s
path, which it prints.

Then, from your project:

```bash
mcode                          # interactive session
mcode "fix the failing test"   # one task, then exit
mcode exec --json "…"          # machine-readable event stream
```

> [!TIP]
> Run `mcode --help` for the flag surface, and `/help` inside a session for the
> command palette. `--yolo` skips approval prompts. It does **not** disable the
> hard-deny floor.

<details>
<summary><strong>Build from source instead</strong></summary>

mcode is a C++23 project, so this route needs a compiler and
[Conan 2](https://conan.io/). The bootstrap script does the three things that are
easy to get wrong by hand: it activates the MSVC developer environment, builds the
private Luau package, and selects the right Conan profile.

**Windows**

```powershell
python -m pip install "conan==2.32.0"
pwsh -File scripts/bootstrap.ps1
cmake --build build/Release
ctest --preset windows-msvc
./build/Release/bin/mcode.exe
```

**Linux / macOS**

```bash
python3 -m pip install "conan==2.32.0"
./scripts/bootstrap.sh
cmake --build build/Release
ctest --preset linux-gcc        # or macos-clang
./build/Release/bin/mcode
```

</details>

## See it in action

```text
$ mcode

❯ refactor the config loader to return std::expected

✻ The loader returns bool and swallows the error code, so callers cannot tell
✻ which failure they got. std::expected carries it without changing the call sites.

✓ read src/mcode/support/config.cxx  0.3s
✓ grep "return false"  0.2s

  Three call sites need the new return type, then I'll build.

✓ edit src/mcode/support/config.cxx  0.4s
✓ edit src/mcode/cli/session.cxx  0.3s

mcode wants to run:
    cmake --build build/Release
  rule: default: exec
  [y] allow once   [a] always allow (saved to this project's store)
  [n] deny once   [d] never allow (this session)   [?] details

✓ bash cmake --build build/Release  8.1s

> /context
▸ /context        Show context-window usage: used, capacity and percent
  /model          Show the model this session runs
  /export         Write the session transcript to a Markdown file
▼ /mention        Pick a workspace file to mention as @path

combo/deepseek ▸ 12.4k tok 8% ▸ $0.031 ▸ 4.2s
```

<details>
<summary><strong>What the TUI does</strong></summary>

An inline renderer. The committed transcript lives in your terminal's own
scrollback, and a bounded live region at the bottom is repainted by a cell-level
diff. `SIGKILL` loses at most the live region, and copy-paste and terminal search
work the way you expect.

- Streaming markdown. Headings, lists, fences and inline emphasis render as
  they arrive, then commit as the rows you saw.
- Tool rows. One live row per call, collapsing to one committed line with the
  target and duration.
- Palette. `/` filters commands, `@` opens a fuzzy file picker, `Ctrl+R`
  searches your session history. Ghost text completes at the caret, and a list
  longer than ten rows scrolls with `▲` and `▼` marking what is off screen.
- Scrollback. PageUp/PageDown and the wheel page history inside the region,
  with `End` to return to live. The terminal's own scrollback is never written to
  or cleared.
- Status line. Model, tokens, context percentage (warn at 80%, error at 95%),
  spend, elapsed, and the current phase with an `esc to interrupt` hint.
- Approval in the UI. The permission prompt renders in the live region and
  reads the same console, with option labels that state their scope.
- Resize-safe. A shrink or grow re-lays the region and clears ghost rows.

</details>

## Tools

Eight tools are compiled into the harness. They are the ones where an invariant
has to hold no matter which model is driving, so the harness enforces it rather
than the prompt.

| Tool | Permission class | What it does |
|---|---|---|
| `read` | read | Read a file with an offset and a limit. Refuses binaries, caps the size, and stays inside the workspace boundary. |
| `edit` | write | Replace an exact string. Requires a prior read of the same bytes and a unique anchor. |
| `write` | write | Create or overwrite a whole file, with the same read-before-write check and size cap. |
| `glob` | read | Match path patterns, with an expansion budget. |
| `grep` | read | Regex content search, gitignore-aware, with result caps. |
| `bash` | exec | Run a command under the OS sandbox, the permission gate and a PTY. |
| `ask_user` | read | Stop and ask when the request is ambiguous, instead of guessing. |
| `tool_search` | read | Find and load the schema of a deferred tool. |

Everything else arrives as an extension, in the same registry and with the same
schemas, permission classes and result types:

| Tool | Extension | What it does |
|---|---|---|
| `task` | `task` | Fan out to a read-only subagent, with its budget carved from the parent's. |
| `todo` | `fs-extra` | Track the plan across the run. |
| `skill_read` | `skills` | Read a discovered skill. |
| `memory_search` | `memory` | Search your own notes ledger. |
| `web_search`, `fetch` | `web` | Search the web, and fetch a URL the model chose, per-domain gated. |
| `git_status`, `git_diff`, `git_commit` | `git` | Workflow tools that compose `bash`. |
| `gh_pr`, `gh_issue`, `gh_review` | `github` | Pull requests, issues and reviews through the `gh` CLI. |

A tool is a Luau table, so `task` and `todo` are not special-cased in the loop.
`tool_search` is what keeps the always-loaded schema inside its budget: a deferred
tool costs nothing until the model asks for it.

## Extensions

Luau, written without a compiler, loaded in ~0.4 ms:

```lua
-- ext.toml: permissions = ["fs_read"]

mcode.tool.register({
	name = "todo_scan",
	description = "Find TODO markers in the workspace",
	schema = { type = "object", properties = { path = { type = "string" } } },
	permission = "read",
	run = function(args)
		local hits = mcode.fs.grep({ pattern = "TODO", path = args.path or "." })
		return { count = #hits, hits = hits }
	end,
})
```

A tool lands in the same registry as the core tools, so the agent's dispatch path
cannot tell where it came from. Extensions can also subscribe to hooks, register
model providers as data, add slash commands, and run timers, all behind a
capability check that fails closed.

Nine extensions ship with the binary: `task`, `fs-extra`, `skills`, `memory`,
`web`, `git`, `github`, plus `providers` (the `openai-chat-completions`,
`openai-responses` and `anthropic-messages` descriptors) and `deepseek-guard`.
The Lua surface is fixed at api version 1: a name is added in a later version
and never removed or renamed, so an extension written today keeps loading. CI
type-checks the shipped stub against the same Luau commit the package pins, so a
VM bump cannot silently invalidate it. The extension-facing names added in 0.1.0
are `mcode.bash` for a command and `mcode.fetch` for a URL the model chose, both
routed through the core handlers so the sandbox and the permission gate cannot be
bypassed from Lua.

## Commands

Inside a session, `/` opens the palette. Nineteen commands ship: `/help`, `/cost`,
`/context`, `/model`, `/tools`, `/compact`, `/remember`, `/export`, `/undo`,
`/rewind`, `/init`, `/extensions`, `/new`, `/continue`, `/resume`, `/doctor`,
`/mention`, `/prompt`, `/exit`. Extensions can add their own, and they appear in
the same list.

`mcode exec` carries the flag surface for scripted use:

```text
--json                 emit one JSON object per line on stdout
--model <name>         model to use
--cwd <path>           working directory
--max-steps <n>        stop after n steps
--max-budget-usd <n>   stop after spending n USD
--approval <mode>      never | on-request | always (default never)
--ask                  restore prompting for every mutating action
--plan                 read-only: refuse edits and commands
--continue, -c         reopen the newest session for this workspace
--resume <id>, -r <id> reopen the session with this id (see --sessions)
--sessions             list this workspace's sessions and exit
--worktree [name], -w  run in a fresh git worktree under .mcode/worktrees/
--add-dir <path>       add an extra workspace root for this run
--no-extensions        disable every extension
--yolo                 skip approval prompts; the hard-deny floor and
                       permissions.deny still apply; spawned commands run
                       under the OS sandbox where the platform supports it
-v, --verbose          more diagnostics on stderr
```

Exit codes are distinct so CI can branch on them: `0` completed, `1` verification
failed, `2` usage error, `3` budget, `4` provider error, `5` permission denied,
`130` interrupted.

## What's in the box

| Area | State |
|---|---|
| Agent loop | plan → act → observe → verify, with reflect, replan and thrash guards; three-axis budget (steps, tokens, USD) |
| Core tools | `read`, `edit`, `write`, `glob`, `grep`, `bash`, `ask_user`, `tool_search` |
| Permissions | Hard-deny floor ahead of every rule and ahead of `--yolo`; project and session rule stores; approval prompt inside the TUI; a taint gate that holds back out-of-workspace actions once untrusted content is in the session |
| Sandboxing | OS primitives on all three platforms: Linux and macOS sandbox profiles, Windows restricted tokens and job objects; egress denied by default, with an allowlist proxy for when it is enabled |
| Extension trust | A repository's `.mcode/extensions/` is inert until a grant matches its tree digest; the bytes hashed are the bytes executed, so there is no TOCTOU window |
| Luau extensions | Versioned `mcode.*` surface, hooks with veto, timers, net with a host allowlist, provider descriptors, per-VM memory and time budgets |
| MCP | stdio and Streamable HTTP transports, supervisor with restart and backoff, server tools flattened into the one registry, plus `resources/*` and `prompts/*` |
| Skills | `AGENTS.md` chain with closest-wins precedence, skill discovery and a budgeted routing index |
| Subagents | Read-only fan-out: the child's registry is built from the read class, so a write call fails schema validation rather than reaching an approval prompt |
| Memory | A user-authored notes ledger, `/remember` to append, `memory_search` to grep it; the ledger lives outside the workspace and no model writes to it |
| Model layer | Streaming SSE, delta assembly, tool-call fragments, retry policy, prompt-cache alignment, provider descriptors as data, tiering by call site (`model.tier.plan|act|side`) |
| Context | Tool-result clearing before compaction, a pinned state block at the front of the prompt, and a meter that reads window fill rather than cumulative spend |
| TUI | Inline cell-diff renderer, streaming markdown, palette, mention picker, history search, bracketed paste, in-region scrollback |
| Sessions | Append-only JSONL event log per workspace; `--continue`, `--resume`, `--sessions`, `--worktree` |
| Verification | `--verify <command>` arms a gate the harness runs itself; the run reports `verified`, `failed` or `no-tests`, and `mcode eval` prints `false_success_rate` |
| Observability | Typed event log, `exec --json` for editors and CI, eval suite |

**Not built yet:** OSC 52 clipboard, image input, background jobs, LSP, ACP, and
the `/skill`, `/mcp`, `/session`, `/reload` commands.

Trust is enforced in the harness rather than requested in the prompt. A
hard-deny floor sits ahead of every rule and ahead of `--yolo`, so a permission
the user never granted cannot be talked into existence. `--verify` arms a gate
the harness runs itself, so a run that claims success without passing it is
reported as `no-tests` rather than as done. A shell-only agent has no typed tool
layer, so edits are not anchored, reads are not bounded, and every action is
gated by string matching.

## Docs

The design is the authoritative specification. Where code and docs disagree, the
docs win. Start at the index.

- [Design index](docs/00-index.md): reading order for every design doc
- [North Star](docs/01-north-star.md): thesis, hard budgets, principles, non-goals
- [Prior art](docs/02-prior-art.md): 16 harnesses compared, plus the Lua-extension precedent
- [Architecture](docs/03-architecture.md): process model, layers, core types, extension seams
- [Agent loop](docs/04-agent-loop.md): state machine, termination, test-time compute
- [Context engineering](docs/05-context-engineering.md): budgeting and compaction
- [Core tools](docs/06-tools.md), [MCP](docs/07-mcp.md), [Skills](docs/08-skills-and-agents-md.md)
- [Security](docs/12-security.md): trust boundary, permission model, sandboxing
- [CLI & TUI](docs/13-cli-and-tui.md): renderer, theme, commit protocol
- [Lua runtime](docs/17-lua-runtime.md), [Lua API](docs/18-lua-api.md), [Extensions](docs/19-extensions.md)
- [Stack](docs/14-cpp23-stack.md), [Providers](docs/15-model-layer.md), [Cross-platform](docs/24-cross-platform.md)
- [Measurements](docs/28-b-measurements.md): extension load, RSS, hook dispatch, measured
- [Non-obvious constraints](docs/40-non-obvious-constraints.md): the traps, numbered
- [Roadmap](docs/16-roadmap.md): milestones and risks

[`CHANGELOG.md`](CHANGELOG.md) records what changed in each release.

## Architecture

```
src/mcode/
├── core/       error model, tool registry, version
├── support/    logging, JSON, TOML, Unicode, config
├── ext/        the Luau extension host
├── fs/         workspace boundary and file primitives
├── net/        SSE parser, HTTP client
├── proc/       subprocess, PTY
├── perm/       the permission engine and stores
├── mcp/        MCP client and supervisor
├── skills/     discovery, frontmatter, routing index
├── instruct/   the AGENTS.md instruction chain
├── model/      provider descriptors, request rendering, capabilities
├── tools/      the core tool set
├── tui/        terminal layer, cell buffer, renderer
├── events/     the event bus
├── agent/      loop, session budget, event log
└── cli/        command surface
```

Dependencies point downward only, from `agent/` toward `core/`. The core knows
four abstractions (`ModelClient`, `Context`, `ToolRegistry`, `EventBus`) and
nothing about Lua, MCP, skills, or the terminal.

## Development

### Dependencies

Everything comes from Conan 2 except Luau, which mcode builds from its own
recipe. See [`docs/14`](docs/14-cpp23-stack.md) for why each was chosen. Size is
the binding constraint: a new dependency must justify itself against measured
size and compile cost, not against an arbitrary count.

| Component | Version | Role |
|---|---|---|
| Luau | `0.0.0-mcode.c0e346ed` (commit `c0e346ed`) | Extension layer |
| yyjson | 0.12.0 | JSON |
| Boost | 1.91.0 | Beast (HTTP/SSE), Process v2 (subprocess) |
| fmt / spdlog | 12.1.0 / 1.17.0 | Formatting, logging |
| mimalloc | 3.5.1 | Allocator |
| simdutf | 9.0.0 | UTF-8/UTF-16 |
| ankerl::unordered_dense | 5.0.1 | Tool registry |
| OpenSSL | 3.5.7 | TLS for the model transport (static) |
| Catch2 | 3.16.0 | Tests |

### Building

```bash
cmake --preset windows-msvc   # or linux-gcc / macos-clang
cmake --build build/Release
ctest --preset windows-msvc
```

Presets are defined in `CMakePresets.json` and consume the Conan toolchain, so
`conan install` must run first. `-DMCODE_WARNINGS_AS_ERRORS=ON` for a strict
build, and the `dev` preset turns LTO off for faster iteration.

> [!NOTE]
> **MSVC is only on `PATH` inside a developer prompt.** Either run
> `scripts/bootstrap.ps1` (which activates it) or start a "Developer PowerShell
> for VS". A MinGW `link.exe` on the same machine will otherwise shadow the linker
> and produce confusing errors.

### Verification

The smoke test proves the toolchain works end to end: every dependency, the Lua
boundary, the workspace boundary, and the agent loop. Its exit code is the number
of failed checks, so CI gates on it directly.

```bash
./build/Release/bin/mcode --smoke
```

CI runs the full suite on Windows and Linux, and build + smoke on macOS. The
strict warning set is on everywhere; `_clgate.py` also syntax-checks every
translation unit against a second toolchain, because the Windows build defines
`_WIN32` and never parses the POSIX branches.

### Platform support

Windows, Linux, and macOS are all tier-1. Every abstraction that touches the OS
(process spawn, PTY, sandbox, paths, file watching) is designed behind an
interface, because retrofitting a platform later is how a project ends up with a
Windows-shaped core.

| Platform | Toolchain | Linking |
|---|---|---|
| Windows x64 | MSVC 19.4x | `/MT`, fully static |
| Linux x86_64 | GCC 14+ or Clang 18+ (libstdc++) | musl static for release |
| macOS arm64 | Apple Clang 16+ | Cannot be fully static: Apple requires a dynamic `libSystem` |

### Contributing

`AGENTS.md` is the authoritative rule set, and it applies to humans as much as to
agents. Read [`CONTRIBUTING.md`](CONTRIBUTING.md) for setup, the style rules that
are load-bearing in review, and the two things CI catches that no local build
will: the POSIX branches, which a Windows workstation never compiles, and the
warnings-as-errors set, which is off locally and on in CI.

Two checks are worth running before every push:

```bash
python _clgate.py                     # clang++ with the CI Clang warning set, in seconds
./build/Release/bin/mcode --smoke     # exit code is the number of failed checks
```

Every trap that cost real time to find is recorded in
[`docs/40`](docs/40-non-obvious-constraints.md), numbered, with the reason it
exists. Docs cite them by number. Read the one you were sent to.

## Security

Report vulnerabilities privately through
[GitHub Security Advisories](https://github.com/immutex/mcode/security/advisories/new),
never in a public issue. [`SECURITY.md`](SECURITY.md) has the threat model, what
is in scope, and a table of which controls actually ship, since the design docs
describe the target and the tree is at `0.1.0`.

## License

Apache-2.0, see [`LICENSE`](LICENSE).

mcode embeds Luau, which is distributed under the MIT License. Luau is
Copyright (c) 2019-2025 Roblox Corporation and Copyright (c) 2005-2019 Lua.org,
PUC-Rio. The full text ships in the Conan package under `licenses/` as
`LICENSE.txt` and `lua_LICENSE.txt`, and is reproduced in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

Upstream asks that products embedding Luau carry attribution for the language
and a link to <https://luau.org/> in their documentation. This section and the
notices file satisfy that request.
