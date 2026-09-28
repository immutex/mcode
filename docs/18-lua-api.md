# Luau Extension API

> TL;DR: One top-level `mcode` table with **22 entries, frozen at `mcode.api_version = 1`**, two-level namespacing, integer ids (never C++ pointers) as handles, `value, err` for environmental failure and `error(msg, 2)` for contract violation, and veto-capable hooks that run synchronously on the loop thread under a 50 ms budget.
>
> **Frozen means frozen.** The surface must be complete before the VM is sealed, because `luaL_sandbox` makes the `mcode` table readonly and Luau enforces readonly on every C API write path — there is no host bypass. Adding an entry after sealing is impossible, not merely discouraged (`12` §Layer 3).

## Design constraints

| Constraint | Value | Why |
|---|---|---|
| Top-level entry points | **22, frozen** | Every exposed name is a forward-compatibility commitment forever, and sealing makes it physically irreversible. Kong's PDK scales via namespaces, not a wide root |
| Namespacing | two-level from day one (`mcode.tool.register`) | Flat modules (`mp.*`, `hs.*`) work to ~40 names then sprout domain prefixes anyway |
| Private namespace | `mcode._*` reserved, undocumented, no stability guarantee | Neovim's `nvim__x` precedent |
| Versioning | integer `mcode.api_version` + `mcode.capabilities` set | Feature detection beats version sniffing (Neovim `api_level`, Kong `version_num`) |
| Change policy | additive-only post-1.0; never rename | Neovim's `vim.loop`→`vim.uv` rename broke downstream distros |
| Handles | integer ids backed by host-side `lua_ref` | Pointers leak lifetime; see "What not to expose" |
| Boundedness | **every entry returns within a stated budget** | The VM interrupt cannot preempt inside a host call, so an unbounded entry defeats the kill switch (`12` §Layer 3) |
| Dialect | **Luau, Lua 5.1-based** — no `//`, no `utf8` table, no 64-bit integers, no `<close>` | `17`. Lua 5.4-era copy-paste does not run |

## Extension layout

```
.mcode/extensions/<name>/
  ext.toml        # manifest: name, version, api_version, permissions, description
  init.luau       # top-level script, run once at load; registers via mcode.*
  lib/            # modules, reached through the host-injected require
```

**`.luau`, not `.lua`.** Every file the host parses is `.luau`, so `luau-analyze` picks it up without configuration and the dialect is stated by the extension. The name is `init.luau`, matching upstream Luau's module convention — one name in one place, rather than `main.lua` in the docs and `init.luau` in the code.

**Top-level script, not `return { setup = ... }`.** Registration is inherently side-effectful; a returned table would need a second interpretation pass and a documented call convention for no benefit. Every mature host converges here — mpv (`main.lua`), AwesomeWM (`rc.lua`), Hammerspoon (`init.lua`). WezTerm's return-a-table works because it has exactly one consumer (config); we have many (tools, commands, hooks, timers).

**`require` is host-provided.** The Luau VM ships no `require` at all. `Luau.Require` is the upstream answer, and it is **rejected here**: it walks the filesystem looking for config, and executes what it finds. mcode resolves modules itself, against the extension root and nowhere else.

| Property | Value |
|---|---|
| Search path | The extension's own `lib/` only. No parent walk, no user scope, no `package.path` |
| Path form | Relative to the extension root (`require("./lib/util")`), with `init.luau` resolving a directory |
| Config files | **None.** No `.luaurc`, no `.config.luau`, nothing executed during resolution |
| Caching | Cached by resolved path, so a module body runs once per extension thread |
| Cycles | Detected; refused with the require chain rather than a stack overflow |
| Escape | A path resolving outside the extension root is a load error, checked after symlink resolution |

**Why upstream's Require is rejected.** `Luau.Require` pulls in `Luau.Config`, and `Luau.Config` does not merely parse: `executeAndExtractConfig` compiles `.config.luau` and runs it with `lua_resume` (source-verified in `Config/src/LuauConfig.cpp`). `RequireNavigator` calls it while walking for config files. So linking upstream's Require means **any `.config.luau` on the resolution path executes** — a second code path, triggered by discovery, that the hash-pinned trust gate does not cover. It is bounded by an interrupt callback, which makes it safe from hangs and irrelevant to the trust question.

Our resolver is ~80 lines, resolves only inside the extension root, executes no config, and is covered by `A7`'s suite. Upstream's is better tested; it is still the wrong trade.

**`init.luau` must be cheap.** It registers and returns; heavy work goes behind `mcode.on("session.start")` or a first tool call. This is the lazy-loading discipline that keeps startup in single-digit milliseconds (lazy.nvim evidence: registration must be sub-millisecond).

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
| `mcode.ext.name` | the calling extension's own name, as a string | Needed for self-attribution without a `debug` API that we removed |
| `mcode.model.register(def)` | declare a provider: endpoint, auth, and the JSON-pointer mapping from its stream to the canonical event model (`15` §Provider seam). **Requires `net`.** The hot path stays in C++; this is data, not a callback | `26` decision 2 |

**That is 23 rows: 26 callable names and 3 fields, and v1 is closed.** `require` is deliberately *not* in this table — Luau treats it as a language-level global, so the host injects it as one. It is a capability all the same, and it is listed under "Extension layout" above. Anything beyond this — string helpers, path manipulation, table utilities — belongs in a pure-Luau companion library, **not** the C-exposed surface. Neovim's `vim.*` utility creep is the cautionary case.

Deliberate omissions, each for a stated reason:

| Not exposed | Because |
|---|---|
| `mcode.event` (alias of `on`/`emit`) | Aliases for one capability cost surface area and split the documentation |
| `mcode.process` (alias of `spawn`) | Same |
| `mcode.ext.list()` / `.doctor()` | Host-side introspection. An extension asking the host about *other* extensions is a capability with no v1 use case, and it hands a hostile extension a map of what is loaded. These are CLI surfaces (`mcode ext doctor`), not API surfaces |
| `mcode.ext.disable(name)` | Same, and worse: it lets one extension disable another. Disabling is a user action |
| `mcode.session.fork` | Deferred. Branching is a host concern until an extension actually needs speculative work; shipping it now freezes a signature nobody has used |
| Anything async | v1 hooks are synchronous with a budget. A coroutine-based `on` is an open question, not a v1 commitment |

**The asymmetry is the argument.** Every cut entry is addable later — `api_version` is additive-only, and `capabilities` exists for feature detection. The reverse does not hold: an entry shipped and later found wrong can only be deprecated, never removed. So the v1 bar is "proven need", not "probably useful".

### One amendment before release

`mcode.model.register` was added after the initial freeze, because `C1` closed the surface before `D2`/`D3` defined the provider seam. It is not a loosening of the rule — it is the rule working: the entry was added while `api_version = 1` had never shipped, so nothing published depends on the old count.

The lesson is recorded rather than glossed: **a freeze declared before the feature that needs it is premature.** The surface was frozen at 22 while `26` still listed the provider seam as an open track. Freezing after `D2` would have produced 23 the first time.

## Capability model

**Default deny.** The manifest declares permissions; absent means none. Every gated entry is checked at call time against the *calling* extension's manifest — never the loading extension's, and never a session-wide setting.

| Permission | Gates | Denied means |
|---|---|---|
| *(none)* | `api_version`, `capabilities`, `ext.name`, `log.*`, `notify`, `cfg.get`, `on`, `off`, `emit`, `defer`, `timer.*`, `tool.register`, `tool.unregister`, `cmd.register`, `skill.register`, `require`, `session.snapshot` | — always available |
| `fs_read` | `fs.read` | `nil, "permission denied"` |
| `fs_write` | `fs.write` | `nil, "permission denied"` |
| `net` | `net.get`, `net.search`, `model.register` | `nil, "permission denied"` |
| `spawn` | `spawn` | `nil, "permission denied"` |
| `mcp` | `mcp.register` | `nil, "permission denied"` |
| `context` | `context.add_instructions` | `nil, "permission denied"` |
| `session_fork` | `session.fork` | `nil, "permission denied"` |

**A denied call is environmental failure, not a contract violation** — it returns `nil, err`, matching `fs.read` on a missing file. A permission check that throws would turn a policy decision into an error path the extension cannot handle gracefully.

`tool.register` is unprivileged on purpose. A tool is a *declaration*: it lands in the registry and, when the model calls it, the call runs through the same permission engine as a built-in (`12` §Decision table). Gating registration would be redundant and would push the check to the wrong layer. What a tool may *do* is gated by the extension's other permissions, not by its right to exist.

### Enforcement primitives

Named, because "the host checks it" is not an implementation:

| Concern | Primitive |
|---|---|
| Globals and libraries readonly; safe-env for constant folding | `luaL_sandbox`, called **after** the API surface is registered |
| Per-extension private globals | `luaL_sandboxthread` on a dedicated thread |
| Thread lifetime across reload | `lua_ref` / `lua_unref` |
| Memory ceiling and per-extension accounting (`B3`) | a custom `lua_Alloc` counting bytes, refusing past the limit |
| Time budget, hang kill | `lua_callbacks()->interrupt`, plus a host watchdog on the wall clock |
| Host userdata destructors that must not re-enter | `lua_newuserdatadtor` |

### Every entry is bounded

The interrupt cannot preempt *inside* a host call, so an unbounded entry defeats the kill switch (`12` §Layer 3). Each entry states a ceiling:

| Class | Entries | Bound |
|---|---|---|
| Constant time | everything except the rows below | No I/O, no input-proportional allocation; returns immediately. `model.register` validates and stores a descriptor; it performs no request |
| Filesystem | `fs.read`, `fs.write` | Byte cap (32 MiB default); workspace-scoped path resolution |
| Network | `net.get`, `net.search` | Connect + total timeout; response byte cap; egress allowlist |
| Process | `spawn` | Wall-clock timeout, killed on expiry; stdout/stderr caps |
| Session | `session.fork` | Append-only; O(1) in history length |
| Host-mediated | `mcp.register`, `context.add_instructions` | Validated and stored; no execution, no I/O |

**A new entry without a stated bound is a defect, not an oversight.** The bound is part of the frozen surface: it is what the 50 ms veto budget (`18` §Events and hooks) and the per-extension CPU accounting are computed from.

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
| Veto handlers are synchronous | Run on the loop thread under a **50 ms wall-clock budget**, enforced by the **VM interrupt plus a host watchdog** — not by an instruction count hook, which Luau does not have. Breach ⇒ handler detached + logged (Neovim `ui_attach` forced-detachment precedent) |
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
| Repeated handler failure | Per-extension error counter trips quarantine after **5 consecutive failures**. Quarantine detaches every handler for that extension and reports it in `mcode ext doctor`; it clears on `/reload`, on a manual re-enable, or after a 60 s backoff with no further errors |
| Slow handler | Watchdog breach detaches the handler and counts toward the same error counter. A detached handler is not retried within the session |

**Errors never propagate into the loop.** An extension error is logged and the session continues — mpv (thread per script), AwesomeWM (`protected_call`), and Neovim (pcall around callbacks) all agree.

### How a handler is protected

Every host→extension call goes through `lua_pcall` **with a message handler** installed beneath the callee, so the traceback is captured at the point of failure rather than after unwinding:

```cpp
lua_pushcfunction( thread, traceback_handler, "traceback" );   // index: handler
const int handler_index = lua_gettop( thread );
lua_pushvalue( thread, function_index );                       // function
lua_pushlstring( thread, payload_json.data(), payload_json.size() );
const int outcome = lua_pcall( thread, 1, 1, handler_index );
lua_remove( thread, handler_index );
```

`traceback_handler` calls `debug.traceback`, which is one of the two `debug` functions Luau retains (`12` §Layer 3). Without the handler, `lua_pcall` returns the bare error string and the stack is already gone — which is the difference between `file:line` and "attempt to index nil".

**The interrupt is per-thread; the callback is not.** `lua_callbacks()->interrupt` lives on the shared global state and fires for whichever thread is running, passing that thread as `L`. A single global deadline would therefore let extension A's watchdog fire inside extension B. The handler must resolve `L` to its owning extension and consult **that** extension's deadline:

```cpp
static void interrupt( lua_State* thread, int gc ) {
    if ( gc >= 0 ) return;                        // GC steps share this callback
    auto* watchdog = watchdog_for( thread );      // thread -> extension, via the registry
    if ( watchdog == nullptr || !watchdog->armed ) return;
    if ( std::chrono::steady_clock::now() < watchdog->deadline ) return;
    watchdog->expired = true;
    luaL_error( thread, "extension exceeded its time budget" );
}
```

Two consequences that are easy to get wrong:

- **Granularity is safepoints, not instructions.** The interrupt fires at 8 opcodes (`12` §Layer 3) — loop back edges, calls, returns. It cannot count instructions, so the budget is wall-clock and checked at safepoints. A 50 ms budget means "checked at the next safepoint after 50 ms", not "stopped at 50 ms".
- **`luaL_error` unwinds as a normal error**, so the breach surfaces to the caller as a failed `lua_pcall` — indistinguishable from any other handler error, which is what makes `pcall` the containment mechanism rather than a special case.

**The budget cannot preempt a host function.** If `mcode.spawn` is running, the VM is not executing, so no safepoint is reached. This is why every entry states a bound (`18` §Every entry is bounded): the extension's own code is interruptible, the host's is not.

### Attribution

Format, fixed because it is grepped and asserted:

```
[extname] file:line: message
```

| Piece | Source |
|---|---|
| `[extname]` | The extension's manifest `name`, from the registry, never from the error text |
| `file:line` | From `debug.traceback`, with the `[string "..."]` prefix rewritten to the extension-relative path |
| `message` | The error value, `tostring`'d if it is not a string |

Tracebacks are normalized before display: newlines collapsed, control characters stripped, absolute host paths rewritten to extension-relative ones. Raw Lua tracebacks have produced rendering bugs in multiple hosts, and leaking host paths into model-visible text is both noise and a small information leak.

**Attribution is automatic and non-optional.** Kong's per-plugin log prefix is the gold standard; Neovim's missing notify-source spawned an entire third-party tool to answer "who called this?". The extension name comes from the registry, so a handler cannot forge another extension's attribution.

### Failure isolation

| Failure | Behavior |
|---|---|
| `init.luau` throws | Extension skipped, traceback logged; session continues. Named in `mcode ext doctor` |
| Handler throws | `pcall` contains it; subscription kept; counter incremented |
| Handler exceeds its budget | Watchdog breach → detached, logged, counter incremented |
| Counter reaches 5 consecutive | **Quarantine**: every handler for that extension is detached |
| Quarantined extension is called again | Returns a value-level failure naming the quarantine; does not re-run |
| Host function fails | Returns `nil, err` (environmental) or raises (contract), per the table above |

Quarantine clears on `/reload`, on an explicit `mcode ext enable`, or after 60 s with no further errors. It is per-extension and never session-fatal.

## Trust model

**Extensions are semi-trusted code with a bounded reach.** The VM removes `io`, `package`, most of `os` and `debug`, bytecode, and write access to `_G`, so an extension cannot reach the host except through the API below. That is a real boundary — but it is a **capability boundary, not an OS sandbox**, and it does not survive a VM engine bug (`12` §Layer 3 owns the limits).

The position, and the one every mature Lua host converges on (Neovim, WezTerm, Hammerspoon, Emacs, mpv):

| Tier | Source | What we do |
|---|---|---|
| **Trusted** | User-installed under `~/.config/mcode/extensions/` | Load in-process, own sandboxed thread. Full API surface. The permission system still gates *tool* execution, not extension code |
| **Semi-trusted** | Project-local `.mcode/extensions/` | **Load only after explicit project-trust confirmation**, with a diff of what would load. A cloned repo must never auto-execute code |
| **Untrusted** | Marketplace / third-party bundles | **Not supported in v1.** The `19` extension-host seam exists so it can be added as a subprocess without a redesign |

What the permission system enforces for extensions: `mcode.fs.*` and `mcode.spawn` are checked against the extension's declared manifest permissions and the session's sandbox/approval policy. Because the VM boundary removes every other route to the filesystem and the process table, this **is** load-bearing rather than decorative — an extension with no declared `fs_write` has no way to write a file. It is still blast-radius control, not a guarantee: a host-side bug in our own `mcode.fs.*` implementation is outside the VM boundary entirely.

## Author tooling

`extensions/mcode.d.luau` is the machine-checkable form of this document. It ships with the harness, and `luau-analyze` type-checks and completes extension code against it.

**It is verified, not decorative.** `luau-analyze` cannot load a definition file — `declare` requires `ParseOptions::allowDeclarationSyntax` with `Mode::Definition`, and the CLI parses every input as an ordinary script. So the check runs through `tools/api_check`, which calls Luau's `Frontend::loadDefinitionFile` directly, and CI asserts two things:

| Fixture | Must |
|---|---|
| `tools/api_check/fixtures/valid.luau` | type-check **clean** — it exercises every documented call shape |
| `tools/api_check/fixtures/invalid.luau` | **fail** — wrong argument types, a missing field, an unknown member |

The second assertion is the one that matters. A definition file that silently degrades to `any` passes the first check and protects nothing; the negative fixture is what makes that failure visible. CI pins the check to the same Luau commit the Conan package pins, so a VM bump cannot quietly invalidate the surface.

### Dialect traps the definition file encodes

Extension authors hit these immediately, so they are stated rather than discovered:

| Trap | Detail |
|---|---|
| `(value, err)` needs **both** on every return path | A function declared `-> (string?, string?)` must return two values. `return value` alone is a type error even when it succeeds. This is the single most common first-run diagnostic |
| `string?` is not `string` | `fs.read` returns an optional, so `#body` on it is an error until checked. `if ok and body then` narrows it; `if ok then` does not |
| No integer subtype | Numbers are doubles. Bit-exact 64-bit work is not available in Luau |
| No `//`, no `utf8`, no `<close>` | Lua 5.4 idioms do not run (`17`) |
| `pairs`/`ipairs` exist; `table.getn`/`maxn`/`foreach` do not | Luau's table library is not 5.1's (`17`) |

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
| A native-call surface as the API | A C→Lua callback costs far more than a Lua→C call; unversionable, undebuggable. Classic `lua_CFunction`s only, and no `ffi` exists to expose |
| Live references to internal state | Copy plain data out (Neovim marshals by copy across the bridge for exactly this reason) |
| The event loop, threads, or raw I/O | `mcode.defer` / `mcode.timer` / `mcode.spawn` only. The loop stays single-threaded |
| Blocking primitives (`os.execute`, synchronous HTTP) | Everything long-running goes through `spawn`/`timer` with cancellation |
| Per-event raw internals (buffers, provider streams) | Typed plain-data payloads only |
| `_G` pollution | `_G` and the library tables are readonly by the VM; each extension also loads on its own sandboxed thread |

## Failure UX

- **Show, don't die.** An extension error is logged with a traceback; the host continues.
- **Attribute automatically.** Every log line and notification carries the extension name.
- **Disable without editing files.** Config-level disable list plus runtime `mcode.ext.disable(name)`, plus auto-quarantine after repeated errors. (Neovim's `vim.g.loaded_*` and mpv's rename-to-`.disable` both require file edits — we can do better.)
- **Degrade to known-good.** A load failure skips the extension and surfaces in `mcode ext doctor`; it never fails the session. WezTerm and AwesomeWM both fall back to a shipped default config — same principle.
- **Tracebacks are formatted.** Normalize newlines and strip control characters before display; raw Lua tracebacks have produced rendering bugs in multiple hosts.

## Open questions

- When (if ever) does registration order stop being enough? Revisit only when extensions actually contend for the same hook.
- Do we need `mcode.on` for *async* handlers (coroutine-based), or is synchronous-with-budget enough? Luau coroutines exist in the VM; the question is whether the dispatch model wants them.
- Is 23 the right number, or does an extension that renders need `mcode.ui.*`? Nothing in the first-party set (`23`) needs it, which is why it is not in v1.

## Sources

- https://raw.githubusercontent.com/neovim/neovim/master/runtime/doc/lua.txt — API levels, `vim.schedule`, result-or-message, bridge copy semantics, autocmd nesting
- https://raw.githubusercontent.com/neovim/neovim/master/runtime/doc/lua-guide.txt — layering, interrupts, user commands
- https://raw.githubusercontent.com/neovim/neovim/master/MAINTAIN.md — deprecation cadence
- https://wezterm.org/config/lua/wezterm/on.html + /config/files.html — ordered callbacks, `false` short-circuit, reload semantics
- https://raw.githubusercontent.com/mpv-player/mpv/master/DOCS/man/lua.rst — `register_event`, `observe_property` coalescing, `add_hook` priority, thread-per-script
- https://awesomewm.org/apidoc/ — signals, `gears.protected_call`, error loop guard
- https://www.hammerspoon.org/go/ — GC lifetime footguns, `hs.showError`
- https://docs.konghq.com/gateway/latest/plugin-development/ — PDK namespaces, `PRIORITY`, `schema.lua`, forward-compat guarantee
- https://github.com/openresty/lua-nginx-module — phase model, `ngx.get_phase`
- https://www.lua.org/manual/5.4/manual.html — error-handling guideline
- https://github.com/luau-lang/luau/blob/master/Require/include/Luau/Require.h — upstream `require`, the configuration callback contract, and why `.config.luau` is executed
- https://luau.org/sandbox/ — what the VM removes, readonly globals, interrupt
- https://luau.org/typecheck/ — gradual typing, `--!strict`, the analyzer
- https://github.com/luau-lang/luau/blob/master/Analysis/include/Luau/Frontend.h — `loadDefinitionFile`, the only way to consume a `.d.luau`
- https://maki.sh/docs/plugins/ — Lua extension precedent, `plugin.toml` permissions
- https://github.com/kfcafe/imp — Lua extension precedent, blocking hooks
