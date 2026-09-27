# Lua Extension API

> TL;DR: One top-level `mcode` table with ~18 entry points, two-level namespacing, integer ids (never C++ pointers) as handles, `value, err` for environmental failure and `error(msg, 2)` for contract violation, and veto-capable hooks that run synchronously on the loop thread under a 50 ms budget.

## Design constraints

| Constraint | Value | Why |
|---|---|---|
| Top-level entry points | ≤ ~20 | Every exposed name is a forward-compatibility commitment forever. Kong's PDK scales via namespaces, not a wide root |
| Namespacing | two-level from day one (`mcode.tool.register`) | Flat modules (`mp.*`, `hs.*`) work to ~40 names then sprout domain prefixes anyway |
| Private namespace | `mcode._*` reserved, undocumented, no stability guarantee | Neovim's `nvim__x` precedent |
| Versioning | integer `mcode.api_version` + `mcode.capabilities` set | Feature detection beats version sniffing (Neovim `api_level`, Kong `version_num`) |
| Change policy | additive-only post-1.0; never rename | Neovim's `vim.loop`→`vim.uv` rename broke downstream distros |
| Handles | integer ids backed by host-side `luaL_ref` | Pointers leak lifetime; see "What not to expose" |

## Extension layout

```
.mcode/extensions/<name>/
  ext.toml        # manifest: name, version, api_version, permissions, description
  main.lua        # top-level script, run once at load; registers via mcode.*
  lua/            # require-able modules, resolved relative to the extension first
```

**Top-level script, not `return { setup = ... }`.** Registration is inherently side-effectful; a returned table would need a second interpretation pass and a documented call convention for no benefit. Every mature Lua host converges here — mpv (`main.lua`), AwesomeWM (`rc.lua`), Hammerspoon (`init.lua`). WezTerm's return-a-table works because it has exactly one consumer (config); we have many (tools, commands, hooks, timers).

**`main.lua` must be cheap.** It registers and returns; heavy work goes behind `mcode.on("session.start")` or a first tool call. This is the lazy-loading discipline that keeps startup in single-digit milliseconds (lazy.nvim evidence: registration must be sub-millisecond).

`ext.toml` is the declarative half — the role Kong's `schema.lua` plays: name, version, `api_version` constraint, permissions, description. Validated at load; a bad manifest fails the *extension*, never the session.

## API surface

| Call | Semantics | Precedent |
|---|---|---|
| `mcode.api_version` | integer, monotonic | Neovim `api_level` |
| `mcode.capabilities` | set of strings for optional features | Neovim `ui_options` |
| `mcode.tool.register(def) -> id` | `def = {name, description, schema, permission, run(args, ctx) -> result, err}`; name collision = load error | Kong load-time validation |
| `mcode.tool.unregister(id)` | deferred if mid-dispatch | `nvim_del_user_command` |
| `mcode.cmd.register(name, fn, opts)` | slash command with optional completion fn | `nvim_create_user_command` |
| `mcode.on(event, fn, opts?) -> id` | subscribe; `opts.once` only — ordering is registration order | Neovim autocmd, `wezterm.on` |
| `mcode.off(id)` | unsubscribe **by id** | mpv's closure-identity unregistration is a known wart |
| `mcode.emit(event, payload)` | fire a custom event; plain data only | `wezterm.emit` |
| `mcode.defer(fn)` | hop onto the loop thread from a callback | `vim.schedule` (the E5560 fix) |
| `mcode.timer.at(ms, fn)` / `.every(ms, fn) -> h` | `h:stop()`; host-anchored, survives GC | mpv `add_timeout`; Hammerspoon's GC footgun is the anti-lesson |
| `mcode.log.debug/info/warn/error(...)` | auto-prefixed with the extension name | Kong's per-plugin log namespace |
| `mcode.notify(msg, level)` | user-visible, attributed | `vim.notify` |
| `mcode.cfg.get(key, default)` | read merged config, validated against the manifest | Kong `schema.lua` |
| `mcode.session.snapshot() -> table` | plain-data copy (ids, counts, current goal); never live references | Neovim bridge copy semantics |
| `mcode.session.fork(at_seq, label) -> branch_id` | Branch the event log at a seq (`04` §Session branching). Append-only: records a `branch.created` event, copies nothing | Needed by `/fork`, `--best-of`, and any extension doing speculative work |
| `mcode.spawn(argv, opts) -> res, err` | subprocess; `res = {exit_code, stdout, stderr}`; async via callback | mpv `utils.subprocess` |
| `mcode.net.get(url, opts)` / `mcode.net.search(query, opts)` | **Gated** HTTP: egress proxy, SSRF filtering (incl. CGNAT and v4-mapped v6), DNS pinning, per-hop redirect revalidation, byte and timeout caps, untrusted-content delimiting. Checked against the calling extension's manifest permissions. The only network access an extension has (`23` §Network split) | The core of the network split: the subsystem is C++, the tool surface is Lua |
| `mcode.fs.read(path)` / `mcode.fs.write(path, data, opts)` | permission-checked, workspace-scoped | `vim.fs`, mpv `utils` |
| `mcode.skill.register(def)` | register a skill: `{name, description, body, path?}`; discovery may also find `SKILL.md` on disk (`08`) | `SKILL.md` convention |
| `mcode.mcp.register(def)` | declare/configure an MCP server: `{name, transport, command/url, tools?}`; tools land in the registry as `mcp__<server>__<tool>` (`07`) | Kong declarative config |
| `mcode.context.add_instructions(text, opts?)` | contribute to the system prompt; **budgeted and counted** (`19`) | Neovim `before_agent_start`-style rewrite, but budgeted |
| `mcode.ext.list()` / `.disable(name)` | introspection and runtime disable | `:checkhealth`, plugin-disable flags |
| `mcode.ext.doctor()` | aggregate per-extension health; extensions may supply `health()` | Neovim `:checkhealth` |

That is 22 entries. Anything beyond this — string helpers, path manipulation, table utilities — belongs in a pure-Lua companion library, **not** the C-exposed surface. Neovim's `vim.*` utility creep is the cautionary case: every name here is a forward-compatibility commitment forever.

Two deliberate omissions from the spec's sketch: there is no `mcode.event` (use `mcode.on`/`mcode.emit`) and no `mcode.process` (use `mcode.spawn`) — aliases for the same capability cost surface area and split the documentation.

## Events and hooks

The event system (`19`) is the spine; the Lua layer is one subscriber among several (TUI, session recorder, logging).

```lua
mcode.on("tool.pre_call", function(ev)
  -- ev = { event = "tool.pre_call", seq = 1234, tool = "bash",
  --        args = { cmd = "rm -rf /" }, session = "s_abc", ext = "myext" }
  if ev.args.cmd:match("^rm%s") then
    return { veto = "destructive shell command blocked by policy" }
  end
end, { priority = 50 })
```

Handler receives **one plain-data event table** (Neovim's autocmd `ev` shape). Rules:

| Rule | Detail |
|---|---|
| Veto is opt-in per event | Only events documented as vetoable (`tool.pre_call`, `spawn.pre`, `prompt.pre`) accept a veto return. **One protocol**: return `{veto = "reason"}`. Bare `false` is accepted as sugar for `{veto = "vetoed by <ext>"}` — the reason is what gets surfaced to the model, so prefer the explicit form |
| First veto wins, and short-circuits | Later handlers for that event are skipped for this dispatch (`wezterm` `return false` short-circuit). Non-veto handlers are never skipped by a peer's error |
| Veto reason is surfaced | Shown to the user and fed back to the model, attributed to the vetoing extension |
| Veto handlers are synchronous | Run on the loop thread under a **50 ms wall-clock budget**; breach ⇒ handler detached + logged (Neovim `ui_attach` forced-detachment precedent) |
| Notification events never veto | `tool.post_call`, `turn.end` — may be coalesced (mpv `observe_property` coalescing) |
| Ordering | **Registration order only.** No priority integers in v1: with three vetoable events and first-veto-wins, order is the whole semantics. mpv and Kong added priority only once composition got genuinely adversarial |
| Reentrancy | Dispatch iterates a **snapshot** of the handler list; register/deregister of the same event during dispatch is deferred; dispatch-depth cap |
| Slow handlers | Notifications may be queued and drained between loop iterations; veto hooks cannot (they gate an action) |

Reentrancy is not theoretical — Neovim has an explicit autocmd-nesting guard (`E218`), WezTerm documents a `config-reloaded` loop, AwesomeWM carries an `in_error` flag against error loops. Snapshot-and-defer plus a depth cap is the cheap general fix.

## Error conventions

Two distinct paths, matching the Lua community guideline ("an exception that is easily avoided should raise an error; otherwise return an error code"):

| Situation | Convention |
|---|---|
| Environmental failure (file missing, spawn failed, permission denied) | `value, err` — assertable, matches `io.open`, Neovim result-or-message, luasocket |
| Contract violation (wrong types, unknown event name, name collision) | `error(msg, 2)` — level 2 attributes the caller (Kong's `error(err, 2)`) |
| Handler throws | Host `pcall`s with `debug.traceback`, logs `[extname] file:line msg`, keeps the subscription |
| Repeated handler failure | Per-extension error counter trips quarantine after N consecutive failures (configurable) |

**Errors never propagate into the loop.** An extension error is logged and the session continues — mpv (thread per script), AwesomeWM (`protected_call`), and Neovim (pcall around callbacks) all agree.

Attribution must be automatic. Kong's per-plugin log prefix is the gold standard; Neovim's missing notify-source spawned an entire third-party tool to answer "who called this?".

## Trust model

**Extensions are trusted code.** LuaJIT's own FAQ states that VM-level sandboxing of untrusted code is not realistic and that process-level isolation is the only promising approach — and `ffi` makes VM-level confinement a fiction regardless (`12` §Lua trust boundary).

The honest position, and the one every mature Lua host takes (Neovim, WezTerm, Hammerspoon, Emacs, mpv):

| Tier | Source | What we do |
|---|---|---|
| **Trusted** | User-installed under `~/.config/mcode/extensions/` | Load in-process. Full API. The permission system still gates *tool* execution, not extension code |
| **Semi-trusted** | Project-local `.mcode/extensions/` | **Load only after explicit project-trust confirmation**, with a diff of what would load. A cloned repo must never auto-execute code |
| **Untrusted** | Marketplace / third-party bundles | **Not supported in v1.** If it ever is: out-of-process host or Luau, not LuaJIT in-process |

What the permission system *does* still enforce for extensions: `mcode.fs.*` and `mcode.spawn` are checked against the extension's declared manifest permissions and the session's sandbox/approval policy. This is a **convenience and blast-radius control, not a security boundary** — an extension can bypass it via `ffi`. Document that plainly rather than implying a guarantee we cannot make.

## Versioning and stability

- `mcode.api_version` is a monotonic integer; per-entry `since` metadata is exposed as `mcode._api_meta`.
- Post-1.0: **additive only** — new events, new optional event fields, new optional `opts` keys. Event field order and semantics are frozen.
- Deprecation: alias + warn for ≥2 minor releases before removal; removal only at a major bump (Neovim's `MAINTAIN.md` cadence).
- Never rename. Add the new name, soft-deprecate the old.
- `mcode._*` is private: no stability guarantee, undocumented.

## What not to expose

| Never | Why |
|---|---|
| Raw C++ pointers or light userdata as **stored** handles | Light userdata is not GC-managed. Use integer ids backed by a host-side `luaL_ref` registry; unregistration is `luaL_unref` |
| Full userdata with `__gc` for host-managed lifetimes | Finalizer reentrancy and use-after-finalize crash class. `__gc` only for mirrors of Lua-owned resources, with a finalized flag |
| An FFI/`cdef` surface as the API | C→Lua FFI callbacks cost ~135 cycles vs ~5 for a Lua→C call (27×); unversionable, undebuggable. Classic `lua_CFunction`s only |
| Live references to internal state | Copy plain data out (Neovim marshals by copy across the bridge for exactly this reason) |
| The event loop, threads, or raw I/O | `mcode.defer` / `mcode.timer` / `mcode.spawn` only. The loop stays single-threaded |
| Blocking primitives (`os.execute`, synchronous HTTP) | Everything long-running goes through `spawn`/`timer` with cancellation |
| Per-event raw internals (buffers, provider streams) | Typed plain-data payloads only |
| `_G` pollution | Each extension loads with a restricted environment; no `package.seeall`-style escapes |

## Failure UX

- **Show, don't die.** An extension error is logged with a traceback; the host continues.
- **Attribute automatically.** Every log line and notification carries the extension name.
- **Disable without editing files.** Config-level disable list plus runtime `mcode.ext.disable(name)`, plus auto-quarantine after repeated errors. (Neovim's `vim.g.loaded_*` and mpv's rename-to-`.disable` both require file edits — we can do better.)
- **Degrade to known-good.** A load failure skips the extension and surfaces in `mcode ext doctor`; it never fails the session. WezTerm and AwesomeWM both fall back to a shipped default config — same principle.
- **Tracebacks are formatted.** Normalize newlines and strip control characters before display; raw Lua tracebacks have produced rendering bugs in multiple hosts.

## Open questions

- When (if ever) does registration order stop being enough? Revisit only when extensions actually contend for the same hook.
- Should `mcode.cmd.register` support completion functions in v1, or defer?
- How many consecutive handler errors before quarantine — and is quarantine permanent or a backoff?
- Do we need `mcode.on` for *async* handlers (coroutine-based) in v1, or is synchronous-with-budget enough?
- Is the ~18-entry budget right, or do we need `mcode.ui.*` for extensions that render?

## Sources

- https://raw.githubusercontent.com/neovim/neovim/master/runtime/doc/lua.txt — API levels, `vim.schedule`, result-or-message, bridge copy semantics, autocmd nesting
- https://raw.githubusercontent.com/neovim/neovim/master/runtime/doc/lua-guide.txt — layering, interrupts, user commands
- https://raw.githubusercontent.com/neovim/neovim/master/MAINTAIN.md — deprecation cadence
- https://wezterm.org/config/lua/wezterm/on.html — ordered callbacks, `false` short-circuit
- https://wezterm.org/config/files.html — config reload semantics
- https://raw.githubusercontent.com/mpv-player/mpv/master/DOCS/man/lua.rst — `register_event`, `observe_property` coalescing, `add_hook` priority, thread-per-script
- https://awesomewm.org/apidoc/ — signals, `gears.protected_call`, error loop guard
- https://www.hammerspoon.org/go/ — GC lifetime footguns, `hs.showError`
- https://docs.konghq.com/gateway/latest/plugin-development/ — PDK namespaces, `PRIORITY`, `schema.lua`, forward-compat guarantee
- https://github.com/openresty/lua-nginx-module — phase model, `ngx.get_phase`
- https://www.lua.org/pil/8.3.html — error-handling guideline
- https://www.lua.org/manual/5.4/manual.html — registry, `luaL_ref`, finalizer hazards
- https://luajit.org/ext_ffi_semantics.html — FFI semantics
- https://maki.sh/docs/plugins/ — Lua extension precedent, `plugin.toml` permissions
- https://github.com/kfcafe/imp — Lua extension precedent, blocking hooks
- https://luajit.org/faq.html — VM-level sandboxing stance
