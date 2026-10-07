<h1 align="center">mcode</h1>

<p align="center">
  <strong>A native C++23 coding-agent harness with a Luau extension layer.</strong><br>
  One native binary. No runtime. A scripting ecosystem that costs nothing until you use it.
</p>

<p align="center">
  <a href="https://github.com/immutex/mcode/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/immutex/mcode/actions/workflows/ci.yml/badge.svg"></a>
  <a href="#license"><img alt="License: Apache-2.0" src="https://img.shields.io/badge/License-Apache--2.0-blue.svg?style=flat-square"></a>
  <img alt="Platforms: Windows, Linux, macOS" src="https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg?style=flat-square">
  <img alt="C++23" src="https://img.shields.io/badge/C%2B%2B-23-00599C.svg?style=flat-square&logo=cplusplus&logoColor=white">
  <img alt="Binary: 8.4 MB" src="https://img.shields.io/badge/binary-8.4%20MB-success.svg?style=flat-square">
  <img alt="Runtime dependencies: 9" src="https://img.shields.io/badge/runtime%20deps-9-success.svg?style=flat-square">
</p>

<details>
<summary><strong>The short version</strong></summary>

Every serious coding harness converged on the same skeleton: one LLM loop over a
small set of typed file and shell tools, OS-level sandboxing, markdown
instruction files, MCP for reach. So "another harness" is a losing frame.

mcode competes on four things the field leaves alone.

**1. Native speed as a product.** Every comparable harness ships a runtime: Node,
Python, Go, or Rust. A statically linked C++23 binary cold-starts in single-digit
milliseconds with a small resident set, which is what makes an agent
*composable*. Scriptable, pipeable, spawnable in parallel, embeddable in CI. A
native Rust harness has been measured at 14.0 ms to first frame and 27.8 MB
resident; a TypeScript one at 590.7 ms and 144.4 MB
(`[THIRD-PARTY]`, [`docs/02`](docs/02-prior-art.md)). Nobody in this space
currently optimizes for it.

**2. A scripting ecosystem in a native core.** No mainstream harness embeds a
scripting language; their extension language *is* their runtime. mcode embeds
Luau, so the extension ecosystem costs under 1 MB, costs nothing when unused, and
extensions are written without a compiler.

**3. Context as a budgeted resource.** Models degrade before their context window
fills, and prefix instability destroys prompt-cache economics. mcode treats
context as a budget with explicit accounting, not a buffer you append to.

**4. Trust over raw capability.** Top-tier resolve rates are saturated and
statistically hard to separate. The remaining headroom is whether the agent can
be trusted not to game the grader, silently shrink scope, or claim success it
never verified.

Everything else (tool names, loop shape, permission axes, TUI idioms) is
deliberately copied from systems that already validated it.

</details>

<p></p>

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

## What makes mcode unique

| | mcode | The field |
|---|---|---|
| **Runtime** | None, just a static binary | Node, Python, Go, or Rust |
| **Extension layer** | Luau VM in-process, ~110 µs marginal per extension | The runtime *is* the extension language |
| **Idle footprint** | ≤ 30 MB budget | 100 MB+ is normal |
| **Cold start** | ≤ 15 ms budget, built for per-invocation spawning | Seconds; designed as a long-lived session |
| **TUI** | Custom ANSI cell-diff renderer, no TUI library | Blessed framework, or plain stdout |
| **Dependencies** | 9 runtime, size is the binding constraint | Whatever the ecosystem drags in |
| **Extension ABI** | A scripting language: versionable, writable without a compiler | A C ABI, `dlopen`, or nothing |

The interesting number is how much the harness can do at that size. A tool is a
Lua table, and a provider is a descriptor.

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
  /mention        Pick a workspace file to mention as @path

combo/deepseek ▸ 12.4k tok 8% ▸ $0.031 ▸ 4.2s
```

<details>
<summary><strong>What the TUI does</strong></summary>

An inline renderer. The committed transcript lives in your terminal's own
scrollback, and a bounded live region at the bottom is repainted by a cell-level
diff. `SIGKILL` loses at most the live region, and copy-paste and terminal search
work the way you expect.

- **Streaming markdown.** Headings, lists, fences and inline emphasis render as
  they arrive, then commit as the rows you saw.
- **Tool rows.** One live row per call, collapsing to one committed line with the
  target and duration.
- **Palette.** `/` filters commands, `@` opens a fuzzy file picker, `Ctrl+R`
  searches your session history. Ghost text completes at the caret.
- **Scrollback.** PageUp/PageDown and the wheel page history inside the region,
  with `End` to return to live. The terminal's own scrollback is never written to
  or cleared.
- **Status line.** Model, tokens, context percentage (warn at 80%, error at 95%),
  spend, elapsed, and the current phase with an `esc to interrupt` hint.
- **Approval in the UI.** The permission prompt renders in the live region and
  reads the same console, with option labels that state their scope.
- **Resize-safe.** A shrink or grow re-lays the region and clears ghost rows.

</details>

<details>
<summary><strong>What an extension looks like</strong></summary>

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

A tool is a table. It lands in the same registry as the core tools, with the same
schemas, permission classes and result types, so the agent's dispatch path cannot
tell where a tool came from. Extensions can also subscribe to hooks, register
model providers as data, add slash commands, and run timers, all behind a
capability check that fails closed.

</details>

## Why another harness?

**vs. Node and Python harnesses.** They are the reason a coding agent needs a
runtime install, a warm start, and hundreds of megabytes resident. mcode is one
binary you can spawn per invocation and pipe.

**vs. Rust harnesses.** Closest in spirit, and they proved the native-speed
thesis. They still have no extension language, so adding a tool means shipping a
build.

**vs. shell-only agents.** Minimal and honest, but a typed tool layer is what
makes edits anchored, reads bounded, and every action permissioned by class
instead of by string matching.

**vs. nothing.** An agent with no budget discipline burns your context window and
your money at the same rate. mcode accounts for both, per turn.

## Quick start

mcode builds from source. It is a C++23 project, so you need a compiler and
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

Then, from your project:

```bash
mcode                    # interactive session
mcode "fix the failing test"   # one task, then exit
mcode exec --json "…"    # machine-readable event stream
```

> [!TIP]
> Run `mcode --help` for the flag surface, and `/help` inside a session for the
> command palette. `--yolo` skips approval prompts. It does **not** disable the
> hard-deny floor.

## Docs

The design is the authoritative specification. Where code and docs disagree, the
docs win. Start at the index.

- [Design index](docs/00-index.md): reading order for all 41 docs
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

## What's in the box

| Area | State |
|---|---|
| **Agent loop** | plan → act → observe → verify, with reflect, replan and thrash guards; three-axis budget (steps, tokens, USD) |
| **Core tools** | `read`, `edit`, `write`, `glob`, `grep`, `bash`, `ask_user`, `tool_search` |
| **Permissions** | Hard-deny floor ahead of every rule and ahead of `--yolo`; project and session rule stores; approval prompt inside the TUI |
| **Sandboxing** | OS primitives on all three platforms: Linux and macOS sandbox profiles, Windows restricted tokens and job objects |
| **Luau extensions** | 28-name frozen API surface, hooks with veto, timers, net with a host allowlist, provider descriptors, per-VM memory and time budgets |
| **MCP** | stdio transport, supervisor with restart and backoff, server tools flattened into the one registry |
| **Skills** | `AGENTS.md` chain with closest-wins precedence, skill discovery and a budgeted routing index |
| **Model layer** | Streaming SSE, delta assembly, tool-call fragments, retry policy, prompt-cache alignment, provider descriptors as data |
| **TUI** | Inline cell-diff renderer, streaming markdown, palette, mention picker, history search, bracketed paste, in-region scrollback |
| **Sessions** | Append-only JSONL event log per workspace; `--continue`, `--resume`, `--sessions`, `--worktree` |
| **Observability** | Typed event log, `exec --json` for editors and CI, eval suite |

**Not built yet:** memory, subagents, OSC 52 clipboard, and the `/skill`, `/mcp`,
`/session`, `/reload` commands. The docs describe them; the tree does not ship
them.

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

## Dependencies

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

## Non-obvious constraints

Every trap that cost real time to find is recorded in
[`docs/40`](docs/40-non-obvious-constraints.md), numbered, with the reason it
exists. Docs cite them by number. Read the one you were sent to.

## Building

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

## Verification

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

## Platform support

Windows, Linux, and macOS are all tier-1. Every abstraction that touches the OS
(process spawn, PTY, sandbox, paths, file watching) is designed behind an
interface, because retrofitting a platform later is how a project ends up with a
Windows-shaped core.

| Platform | Toolchain | Linking |
|---|---|---|
| Windows x64 | MSVC 19.4x | `/MT`, fully static |
| Linux x86_64 | GCC 14+ or Clang 18+ (libstdc++) | musl static for release |
| macOS arm64 | Apple Clang 16+ | Cannot be fully static: Apple requires a dynamic `libSystem` |

## License

Apache-2.0.

mcode embeds **Luau**, which is distributed under the MIT License. Luau is
Copyright (c) 2019-2025 Roblox Corporation and Copyright (c) 2005-2019 Lua.org,
PUC-Rio. The full text ships in the Conan package under `licenses/` as
`LICENSE.txt` and `lua_LICENSE.txt`, and is reproduced in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

Upstream asks that products embedding Luau carry attribution for the language
and a link to <https://luau.org/> in their documentation. This section and the
notices file satisfy that request.
