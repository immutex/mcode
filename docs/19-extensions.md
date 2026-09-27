# Extensions

> TL;DR: Extensions are Lua directories with a manifest, discovered from user and project roots, loaded in-process, and gated by a hash-pinned trust grant; every capability they add — tools, commands, hooks, context — registers through the same `mcode` API and lands in the same registries as the built-ins.

## The extension seam

The agent core knows four abstractions and nothing else: `ModelClient`, `Context`, `ToolRegistry`, `EventBus` (plus `Session`). Everything that adds behavior attaches to one of those.

```
Agent Core
   │
   ├── ToolRegistry ←── BuiltinToolSource
   │                ←── LuaToolSource      (extensions)
   │                ←── McpToolSource      (MCP servers)
   │
   ├── EventBus     ←── Lua hooks, session recorder, TUI, logging
   │
   └── Context      ←── instructions, skills, extension-provided context
```

**The agent never knows where a tool came from.** This is the constraint that keeps the core small while the ecosystem grows — and it is testable: the agent's unit tests must pass with a mock `ToolSource` that has no Lua, no MCP, and no filesystem.

## Extension layout

```
~/.config/mcode/extensions/<name>/     # user scope (trusted)
<project>/.mcode/extensions/<name>/      # project scope (untrusted until granted)
  ext.toml        # manifest
  main.lua        # top-level script, run once at load
  lua/            # require-able modules, resolved relative to the extension first
```

Precedence: **project > user > built-in**, with same-name collisions reported at load rather than silently resolved (an extension shadowing another is a support burden, not a feature).

## Manifest

```toml
name = "git"
version = "0.3.0"
api_version = ">=1"
description = "Git-aware tools and commit message conventions"
permissions = ["fs_read", "fs_write", "spawn"]   # omit ⇒ deny
```

The manifest is the declarative half — the role Kong's `schema.lua` plays. It is validated at load:

| Field | Rule |
|---|---|
| `name` | `^[a-z0-9]+(-[a-z0-9]+)*$`, ≤64 chars, must match the directory name |
| `version` | Semver |
| `api_version` | Constraint against `mcode.api_version`; mismatch ⇒ refuse to load, do not guess |
| `permissions` | Subset of the known capability set; **absent or empty means deny** |
| `description` | ≤1024 chars; used in `mcode ext list` |

A bad manifest fails **the extension**, never the session.

**Permissions are capability declarations, not a sandbox.** They scope what `mcode.fs.*` and `mcode.spawn` will do on the extension's behalf. An extension with `ffi` can bypass them entirely (`12` §Layer 3). They reduce blast radius and make intent reviewable; they are not a security boundary, and the docs must say so.

## Loading lifecycle

```
discover → validate manifest → trust check → hash verify → load → main.lua → registered
```

| Phase | Behavior |
|---|---|
| **Discover** | Scan user and project extension roots; read manifests only. No code executes. Startup cost is filesystem reads |
| **Validate** | Parse `ext.toml`; reject bad names, unknown permission strings, `api_version` mismatch |
| **Trust check** | User-scope: trusted by installation. Project-scope: **inert until an explicit, persisted, hash-pinned grant** (`12` §Repo-shipped extensions) |
| **Hash verify** | Re-hash at every load; a changed file invalidates the grant |
| **Load** | Fresh restricted environment; `mcode` injected; `main.lua` executed |
| **Register** | `main.lua` calls `mcode.tool.register`, `mcode.cmd.register`, `mcode.on`, … Registration must be sub-millisecond |

**Lazy by construction.** `main.lua` registers and returns. Heavy work goes behind `mcode.on("session.start")` or a first tool call. This is what keeps extension count decoupled from startup time — the pattern VS Code codifies as activation events and Neovim as remote-plugin manifests.

Extensions are loaded **after** the loop and event bus exist but **before** the first model request, so hooks are in place for turn one.

## Tool sources

The abstraction that makes tool provenance irrelevant:

```cpp
class ToolSource {
public:
  virtual ~ToolSource() = default;
  virtual std::vector<ToolSpec> tools() const = 0;
  virtual result<ToolResult> call(std::string_view name, const json& args) = 0;
};
```

| Source | Registered at | Namespacing | Notes |
|---|---|---|---|
| `BuiltinToolSource` | Compile time | `read`, `edit`, `bash`, … | Static schemas, zero runtime cost |
| `LuaToolSource` | Extension load | `<ext>__<tool>` | Schema is a Lua table validated at registration |
| `McpToolSource` | Server connect | `mcp__<server>__<tool>` | Deferred loading; schemas arrive at runtime |

Registry invariants: names are unique and collision-checked; every tool declares a permission class; schemas are immutable after registration and their rendered form is byte-stable for prompt caching; the tool count is budgeted, with overflow moving behind `tool_search` (`06`).

## Context contributions

Extensions can contribute to the prompt, but only through declared, budgeted channels:

| Channel | Mechanism | Budget |
|---|---|---|
| Instructions | `mcode.context.add_instructions(text)` at load, or a `prompt.pre` hook | Counted against the instruction budget |
| Skills | Drop a `SKILL.md` directory; discovery handles it (`08`) | ~30 tokens until invoked |
| Dynamic context | `prompt.pre` hook may amend context for one turn | Vetoable event; synchronous with a budget |

There is no "append arbitrary text to every prompt" API. Ungoverned context growth is the most common way a plugin ecosystem degrades a harness, and the budget enforcement has to live in the harness, not the extension.

## Failure, quarantine, and doctor

| Failure | Behavior |
|---|---|
| Manifest invalid | Skip extension; report in `mcode ext doctor` |
| `api_version` mismatch | Skip; name the required version |
| `main.lua` errors | Log with traceback, attributed; skip extension; session continues |
| Handler errors repeatedly | Per-extension error counter → **quarantine** after N consecutive failures (configurable); other extensions unaffected |
| Handler exceeds its wall-clock budget | Detach the handler, log, continue (`18`) |
| Extension hangs or OOMs | **Not containable in-process** — requires the untrusted-tier subprocess (`12`) |

`mcode ext doctor` aggregates per-extension health: load status, registration counts, error counts, last error with traceback, declared vs used permissions. Extensions may supply a `health()` function. This is the `:checkhealth` analog, and it exists because attribution must be automatic — Neovim's missing notify-source spawned an entire third-party tool to answer "who called this?"

Disabling requires no file edits: a config-level disable list plus `mcode.ext.disable(name)` at runtime.

## Hot reload

**You cannot unload Lua state.** `package.loaded[name] = nil` + `require` re-executes the file, but old closures, upvalues, timers, registered callbacks, JIT traces, and cdata from the previous incarnation remain reachable and keep running.

| Command | Semantics | Cost |
|---|---|---|
| `/reload <ext>` | Clear the extension's registrations (tools, commands, hooks, timers), `package.loaded[name] = nil`, re-run `main.lua` in a **fresh `lua_State`** | Milliseconds; the only correct option |
| `/reload` (all) | Rebuild every extension's state | Milliseconds; prefer this to partial reloads |

Reloading in a fresh state per extension is the honest implementation. Reusing the state and re-requiring produces exactly the stale-closure bug class that WezTerm documents and Neovim's guide warns about. Document that reload does **not** reclaim memory from abandoned states — LuaJIT's allocator does not return freed pages to the OS (`17`).

## Distribution

Extensions are directories in a git repository. Install is `mcode ext install git:owner/repo@tag`, which clones and records the **commit SHA in a lockfile**; updates are explicit, diff-showing, and never automatic.

Discovery is an **index, not a registry** — no artifact hosting, no accounts, no review gate. Trust comes from the runtime hash-pinned grant above, not from listing. Full design, trust model, manifest `[registry]` block, and the docs site: `25`.

The honest consequence: **installing an extension is equivalent to running its author's code with your privileges.** Say that plainly next to the install instructions. This is the posture Neovim, mpv, WezTerm, and Hammerspoon take, and pretending otherwise would be worse than the risk itself.

If untrusted distribution ever becomes a goal, it requires the untrusted tier (out-of-process host or Luau) — not a Lua-level sandbox (`12`).

## Traps

- **Auto-loading project extensions.** A `git clone` plus one run would execute attacker code. The trust gate is not optional.
- **Calling permissions a sandbox.** `ffi` bypasses them. Call them capability scoping.
- **Doing work in `main.lua`.** Registration must be cheap; heavy work belongs behind an event.
- **Reloading by re-requiring in the same state.** Stale closures keep running.
- **Silent name collisions.** Report them; do not let one extension shadow another.
- **Ungoverned context injection.** Budget it in the harness or the prompt grows without limit.
- **Assuming an error counter makes extensions safe.** It makes them *survivable*; only a process boundary makes them *contained*.
- **Unbounded `mcode.on` registrations.** An extension that subscribes in a loop degrades every event dispatch; cap registrations per extension and report overages in doctor.

## Open questions

- Per-extension `lua_State` (isolation, cost) versus one shared state (cheap, no isolation)? Leaning per-extension: it makes `/reload` correct and bounds error blast radius.
- Is a fresh `lua_State` per reload worth the memory it abandons, or should we offer a full-process restart path for heavy reload cycles?
- Quarantine threshold: how many consecutive handler errors, and is recovery automatic or manual?
- Do project extensions get a reduced API surface (no `ffi`, no network) even after a trust grant, as defense in depth?
- Should `ext.toml` support declaring required host capabilities (e.g. "needs MCP") so load can fail early with a clear message?

## Sources

- https://neovim.io/doc/user/lua/ — `vim.secure` trust model, plugin discovery, reload semantics
- https://raw.githubusercontent.com/neovim/neovim/master/runtime/doc/lua-guide.txt — `package.loaded` idiom, stale state
- https://raw.githubusercontent.com/neovim/neovim/master/runtime/doc/remote_plugin.txt — deferred host start rationale
- https://raw.githubusercontent.com/microsoft/vscode-docs/main/api/references/activation-events.md — declarative activation, lazy loading
- https://raw.githubusercontent.com/microsoft/vscode-docs/main/api/advanced-topics/extension-host.md — extension host charter
- https://docs.konghq.com/gateway/latest/plugin-development/ — manifest/schema validation, priority
- https://github.com/Kong/kong/blob/master/kong.conf.default — `untrusted_lua` tiers
- https://raw.githubusercontent.com/mpv-player/mpv/master/DOCS/man/lua.rst — directory scripts, per-script thread
- https://wezterm.org/config/files.html — config re-evaluation semantics
- https://maki.sh/docs/plugins/ — Lua extension precedent, `plugin.toml` permissions, `/reload`
- https://github.com/kfcafe/imp — Lua extension precedent
- https://openresty.org/ — `lua_code_cache off` true-unload cost
- https://luajit.org/faq.html — sandboxing stance
- https://blog.openresty.com/en/luajit-plus/ — allocator page retention
- https://www.lua.org/manual/5.4/manual.html — `package.loaded` semantics
