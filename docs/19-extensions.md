# Extensions

> TL;DR: Extensions are Luau extension directories with a manifest, discovered from user and project roots, loaded in-process, and gated by a hash-pinned trust grant; every capability they add — tools, commands, hooks, context — registers through the same `mcode` API and lands in the same registries as the built-ins.

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
  init.luau       # top-level script, run once at load
  lib/            # modules, resolved relative to the extension root only
```

Precedence: **project > user > built-in**, with same-name collisions reported at load rather than silently resolved (an extension shadowing another is a support burden, not a feature).

## Manifest

```toml
name = "git"                  # required
version = "0.3.0"             # required
api_version = 1               # required; integer, the minimum API it needs
description = "Git-aware tools and commit message conventions"
permissions = ["fs_read", "fs_write", "spawn"]   # omit ⇒ deny
```

The manifest is the declarative half — the role Kong's `schema.lua` plays. **Frozen at v1**; the loader rejects anything not in this table:

| Field | Type | Rule | On violation |
|---|---|---|---|
| `name` | string | `^[a-z0-9]+(-[a-z0-9]+)*$`, ≤64 chars, must equal the directory name | Refuse |
| `version` | string | Semver (`MAJOR.MINOR.PATCH`, optional prerelease/build) | Refuse |
| `api_version` | integer | ≥1. Loads iff `mcode.api_version >= api_version` | Refuse |
| `permissions` | array of strings | Subset of `18`'s permission set. **Absent or empty means deny** | Refuse |
| `description` | string | ≤1024 chars; used in `mcode ext list` | Refuse |
| *(unknown key)* | — | Not in the table above | Refuse |

**`api_version` is an integer, never a range.** The extension states the minimum API it needs; the loader compares two integers. Range syntax (`">=1"`, `"~1.2"`) is what `25` calls out as the bug class to avoid — a constraint parser is a parser, and the only question it answers here is whether one integer is at least another. The version *policy* (additive-only, deprecation windows) lives in `18` §Versioning; the manifest only declares a floor.

**Unknown keys are rejected, not ignored.** A typo in `permissions` silently disabling a capability is the failure mode this prevents: `permission = ["fs_write"]` must fail loudly rather than load with no permissions and a confusing denial later. Same rule as unknown CLI flags (`AGENTS.md` §Correctness).

A bad manifest fails **the extension**, never the session.

**Permissions are capability declarations**, and `18` §Capability model holds the exact mapping from each permission to the host functions it gates. Two properties matter here:

- **Default deny.** Absent or empty means no gated function is callable; the extension loads and runs with the unprivileged surface only. A denial returns `nil, err`, never throws.
- **Checked at call time, against the caller.** Not at load, not against a session setting. An extension that calls `mcode.fs.write` without `fs_write` gets a denial it can handle, and the same check applies when its *tool* runs, because tool execution re-enters through the same host functions.

They are load-bearing because the VM boundary removes every other route to the filesystem and the process table — an extension cannot open a file except through `mcode.fs.*`. They are still not a guarantee: a bug in our own `mcode.fs.*` implementation is outside the VM boundary (`12` §Layer 3).

## Loading lifecycle

```
discover → validate manifest → trust check → hash verify → load → init.luau → registered
```

| Phase | Behavior |
|---|---|
| **Discover** | Scan user and project extension roots; read manifests only. No code executes. Startup cost is filesystem reads |
| **Validate** | Parse `ext.toml`; reject bad names, unknown permission strings, `api_version` mismatch |
| **Trust check** | User-scope: trusted by installation. Project-scope: **inert until an explicit, persisted, hash-pinned grant** (`12` §Repo-shipped extensions) |
| **Hash verify** | Re-hash at every load; a changed file invalidates the grant |
| **Load** | Fresh restricted environment; `mcode` injected; `init.luau` executed |
| **Register** | `init.luau` calls `mcode.tool.register`, `mcode.cmd.register`, `mcode.on`, … Registration must be sub-millisecond |

**Lazy by construction.** `init.luau` registers and returns. Heavy work goes behind `mcode.on("session.start")` or a first tool call. This is what keeps extension count decoupled from startup time — the pattern VS Code codifies as activation events and Neovim as remote-plugin manifests.

Extensions are loaded **after** the loop and event bus exist but **before** the first model request, so hooks are in place for turn one.

## Extension host interface

One interface, two implementations. v1 ships the in-process host; the process tier is the same contract over a pipe, so adding it is an implementation, not a redesign (`12` §Layer 3).

```cpp
// Everything crossing this seam is plain data. No pointers, no light userdata,
// no live references -- those cannot be serialized, and admitting them now is
// what would make the process tier a rewrite.
struct extension_id { std::string name; std::string scope; };   // scope: user | project

struct load_result {
    extension_id id;
    std::vector<std::string> registered_tools, registered_commands, subscribed_events;
    std::uint64_t bytes_used;                                  // B3
};

struct extension_stats {
    std::uint64_t bytes_used, peak_bytes, invocations, errors, interrupts;
};

class extension_host {
public:
    virtual ~extension_host() = default;

    // Loads init.luau in a fresh sandboxed thread. Throws nothing; a bad
    // extension fails itself and the session continues.
    virtual auto load( const extension_manifest& manifest ) -> result<load_result> = 0;

    // Drops the host registrations, then the thread. The VM's shared global
    // state survives (see `12` §Layer 3) -- unregistering is what makes the
    // extension actually gone.
    virtual auto unload( const extension_id& id ) -> status = 0;

    virtual auto invoke( const extension_id& id, std::string_view entry, std::string_view args_json )
        -> result<std::string> = 0;

    // Synchronous hook dispatch on the loop thread. Returns whether the handler
    // vetoed, for the Pre* events that allow it (`20`).
    virtual auto dispatch( const extension_id& id, std::string_view event, std::string_view payload_json )
        -> result<bool> = 0;

    virtual auto stats( const extension_id& id ) const -> extension_stats = 0;
};
```

| Aspect | In-process (v1) | Process tier (later) |
|---|---|---|
| `load` | `luaL_sandbox` + sandboxed thread | spawn child, same handshake |
| `invoke` / `dispatch` | direct call through `lua_pcall`, synchronous | request/response over a pipe, with a timeout |
| `stats` | allocator counters, exact | reported by the child, trusted |
| Failure / ceiling | `lua_pcall` contains the error; `lua_Alloc` limit | process death contains everything; Job Object / rlimit |

**Three rules the interface exists to enforce:**

1. **Every method returns a value, never throws** — a host that can throw across the boundary reintroduces the problem `18` §Error conventions solves.
2. **Everything crossing is plain data** — the moment a pointer crosses, the process tier stops being an implementation swap. Same reason `18` forbids pointers as stored handles.
3. **`invoke` and `dispatch` are bounded** — the VM interrupt cannot preempt inside a host call, so an unbounded entry point defeats the kill switch (`12` §Layer 3).

The in-process host is `src/mcode/ext/lua_host.cxx` today, implementing the sandboxed-thread half of this contract. The abstraction lands with the loader (`C5`); nothing above it should reference `lua_State`.

## Tool sources

The abstraction that makes tool provenance irrelevant:

| Source | Registered at | Namespacing | Notes |
|---|---|---|---|
| `BuiltinToolSource` | Compile time | `read`, `edit`, `bash`, … | Static schemas, zero runtime cost |
| `LuauToolSource` | Extension load | `<ext>__<tool>` | Schema is a Luau table validated at registration |
| `McpToolSource` | Server connect | `mcp__<server>__<tool>` | Deferred loading; schemas arrive at runtime |

All three implement one interface — `tools()` returning specs, `call(name, args)` returning a result — and nothing above it knows which is which.

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
| `init.luau` errors | Log with traceback, attributed; skip extension; session continues |
| Handler errors repeatedly | Per-extension error counter → **quarantine** after N consecutive failures (configurable); other extensions unaffected |
| Handler exceeds its wall-clock budget | Detach the handler, log, continue (`18`) |
| Extension hangs | Contained: the host interrupt fires at 8 opcodes plus a wall-clock watchdog (`12` §Layer 3). A call-free straight-line block is the one case that runs to completion first |
| Extension OOMs | Contained by the `lua_Alloc` ceiling — all VM memory routes through it |

`mcode ext doctor` aggregates per-extension health: load status, registration counts, error counts, last error with traceback, declared vs used permissions. Extensions may supply a `health()` function. This is the `:checkhealth` analog, and it exists because attribution must be automatic — Neovim's missing notify-source spawned an entire third-party tool to answer "who called this?"

Disabling requires no file edits: a config-level disable list plus `mcode.ext.disable(name)` at runtime.

## Hot reload

**Re-requiring in place is wrong.** `package.loaded[name] = nil` + `require` re-executes the file, but old closures, upvalues, timers, and registered callbacks from the previous incarnation stay reachable and keep running — the stale-closure bug class WezTerm documents and Neovim's guide warns about.

| Command | Semantics | Cost |
|---|---|---|
| `/reload <ext>` | Unregister everything the extension registered (tools, commands, hooks, timers), then **`lua_resetthread`** its sandboxed thread and re-run `init.luau` | Milliseconds; the correct option |
| `/reload` (all) | Reset and re-run every extension | Milliseconds; prefer this to partial reloads |

`lua_resetthread` is what makes this clean: it closes upvalues, clears call frames and thread state, and clears the stack — a genuinely fresh thread, not a re-`require` over stale closures (`12` §Layer 3).

**Two limits.** The VM's shared global state persists across a reset (interned strings, readonly library tables), so an extension is only truly gone once its thread is reset *and* its host registrations are dropped. And `lua_resetthread` **sets `L->finalizers = nullptr` without running them**, so any host userdata destructor still queued for that thread is skipped and the resource leaks — drain finalizers before resetting.

## Distribution

Extensions are directories in a git repository. Install is `mcode ext install git:owner/repo@tag`, which clones and records the **commit SHA in a lockfile**; updates are explicit, diff-showing, and never automatic.

Discovery is an **index, not a registry** — no artifact hosting, no accounts, no review gate. Trust comes from the runtime hash-pinned grant above, not from listing. Full design, trust model, manifest `[registry]` block, and the docs site: `25`.

The honest consequence: **installing an extension means granting its author every capability the API exposes, bounded by the VM boundary and the manifest.** Say that plainly next to the install instructions. This is the posture Neovim, mpv, WezTerm, and Hammerspoon take, and pretending otherwise would be worse than the risk itself.

If untrusted distribution ever becomes a goal, it requires the process tier — the `19` extension-host seam exists so that is an addition, not a redesign (`12`).

## Traps

- **Auto-loading project extensions.** A `git clone` plus one run would execute attacker code. The trust gate is not optional.
- **Calling the VM boundary a sandbox.** It is an in-process capability boundary and upstream does not claim it is formally proven. Say "capability boundary."
- **Doing work in `init.luau`.** Registration must be cheap; heavy work belongs behind an event.
- **Reloading by re-requiring in the same state.** Stale closures keep running.
- **Silent name collisions.** Report them; do not let one extension shadow another.
- **Ungoverned context injection.** Budget it in the harness or the prompt grows without limit.
- **Assuming an error counter makes extensions safe.** It makes them *survivable*; only the process tier makes a hostile extension *contained*.
- **Unbounded `mcode.on` registrations.** An extension that subscribes in a loop degrades every event dispatch; cap registrations per extension and report overages in doctor.

## Open questions

- Per-extension sandboxed thread (isolation, cost) versus one shared thread (cheap, no isolation)? Leaning per-extension: it makes `/reload` correct and bounds error blast radius. Measure the cost in `B2`.
- Does `lua_resetthread` reclaim enough that per-reload memory growth stays flat across hundreds of reloads? Measure; the abandoned-finalizer leak is the risk.
- Quarantine threshold: how many consecutive handler errors, and is recovery automatic or manual?
- Do project extensions get a reduced API surface (no network) even after a trust grant, as defense in depth?
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
- https://blog.openresty.com/en/luajit-plus/ — allocator page retention
- https://github.com/luau-lang/luau/blob/master/VM/src/lstate.cpp — `lua_resetthread` semantics and the skipped-finalizer trap
- https://luau.org/sandbox/ — the VM boundary
