# Workstream B — The declared surface, made real

> TL;DR: `extensions/mcode.d.luau` declares 23 entry points. **13 of them do not
> exist.** An extension author writes against a type-checked declaration, gets a
> clean bill of health from `luau-analyze`, and fails at runtime. This slice
> implements the missing namespaces — `fs`, `cfg`, `cmd`, `timer`, `session`,
> `net`, `skill.register`, `context.add_instructions` — enforces the manifest
> permissions that gate them, adds `/reload`, and adds the test that makes the
> gap impossible to reintroduce.

## Why this slice

The design's load-bearing claim is *"C++ provides the runtime primitives. Lua
provides the ecosystem."* It cannot be evaluated while the surface is half
declared: the extensions that would prove it — `git`, `github`, `web` — need
`spawn`, `fs`, `cfg` and `cmd`, and none of those exist.

Measured:

| Claim | Reality |
|---|---|
| The API is frozen at 23 entry points | 13 are unimplemented |
| An author can type-check their extension | Yes — and the checker passes for calls that cannot run |
| The manifest's permissions are enforced | `fs_read`, `fs_write`, `net`, `spawn` are declared and validated; nothing gates on them because there is nothing to gate |
| An extension can be reloaded | No `/reload`; the loader has no unload-and-reinstall path for a live VM |
| A broken extension is quarantined | Per-extension error counting exists; quarantine is not wired |

The missing set, exactly:

```
cmd.register      timer.at          session.snapshot    fs.read
cmd.handler       timer.every       session.fork        fs.write
cfg.get           net.get           skill.register
                  net.search        context.add_instructions
```

That list is the slice. It is mostly mechanical — each entry point is a
`lua_CFunction`, an `ENTRIES` row, a declaration, and a test — which is why the
highest-value item is not an implementation at all, but the test that keeps the
declaration file and the implementation in step.

## Scope

**In**

- The 13 entry points above
- Manifest-permission enforcement on every one that needs it
- `/reload`: a fresh VM per extension, stale-closure semantics documented
- Error containment: per-extension counter, quarantine after a threshold
- A **declared-surface test** that fails when the two disagree
- `luau-analyze` clean against the updated definition file

**Out**

- `spawn` as an entry point. It is a manifest *permission*, not a declared
  function; `bash` and `cmd` cover the need, and adding a raw process-spawn API
  is a separate decision with its own security surface
- `task` (subagents), `memory`, `web`/`git`/`github` as bundled extensions.
  They become possible *because* of this slice; writing them is the next
  batch's proof that the API is sufficient
- `context.add_instructions`' full budgeted-pipeline semantics. The plumbing
  lands here; the prompt assembler's per-contributor budget accounting is M2
  work that this slice does not restructure
- Any change to `perm/`, `tui/`, `platform/` or `proc/`

## Design

### Build the disagreement test first

Before implementing anything, write the test that walks
`extensions/mcode.d.luau`'s declarations and fails if any is absent from
`api.cxx`'s `ENTRIES`. It will fail on all 13 today. That failure list is the
worklist, and it is the only mechanical defence against this class of drift —
the same discipline the second batch's `_clgate.py` preflight applied to
platform guards.

Do it in **both directions**: a declared-but-missing entry fails, and an
implemented-but-undeclared entry fails too. `on`, `off` and `emit` are already
in the second category, so the test finds real drift on its first run.

### One file pair per namespace

`api.cxx` is already the largest file in the extension layer. Each new
namespace gets its own `api_<namespace>.{hxx,cxx}` following the
`api_skill`/`api_mcp` precedent from the last batch, with `ENTRIES` rows and
`install_request` fields appended at fixed anchors. That keeps the merge with
the other slices trivial and keeps each file inside the 600-line cap.

The shared helpers (`surface_from`, `read_field_string`,
`read_field_string_array`) already live in `ext/api_internal.{hxx,cxx}` from
the last batch's consolidation. Use them; do not add a fourth copy.

### Permissions are the point, not a formality

The manifest declares `fs_read`, `fs_write`, `net`, `spawn`, `mcp`,
`context`, `session_fork`. Until now nothing enforced them because nothing
needed to. Each entry point below is gated, and **the gate is checked at call
time against the extension's own manifest**, never against a global flag:

| Entry point | Permission | Note |
|---|---|---|
| `fs.read` | `fs_read` | resolves through the workspace boundary; outside-workspace goes to the permission engine, exactly as the file tools do |
| `fs.write` | `fs_write` | same, plus the `.git/`/`.mcode/` deny |
| `net.get` | `net` | domain policy; see below |
| `net.search` | `net` | |
| `session.fork` | `session_fork` | |
| `session.snapshot` | `session_fork` | a snapshot is a branch point |
| `context.add_instructions` | `context` | |
| `cmd.register`, `cmd.handler` | — | presentation, no capability |
| `timer.at`, `timer.every` | — | bounded; see below |
| `cfg.get` | — | read-only, and scoped |
| `skill.register` | — | registers by value; no filesystem reach |

**A denied call returns `nil, err`, never a raise.** That is the existing
convention for `mcp.register` without its permission, and it is the one
extensions can handle.

### `fs` must not become a second file-tool implementation

`fs.read`/`fs.write` resolve through the same workspace boundary, the same
protected-path deny, and the same permission engine as `read`/`write`. The
rulebook is explicit that a duplicated loader that drifts returns a wrong
value, and a second path-resolution implementation is exactly that. Route
through the existing `workspace` and `perm` entry points; do not re-implement
canonicalization.

### `net` needs a domain policy, not just a client

`net.get` and `net.search` reach the network from inside an extension. The
`net/http_client` exists and is tested. What does not exist is a policy: which
hosts an extension may reach.

Keep it minimal and honest for this slice: **an extension may reach only the
hosts its manifest declares** — as `net:<host>` entries in `permissions` — and
the permission engine sees the request as a `net`-class call so `ask`/`deny`
apply. The same declaration gates a `model.register` endpoint, so there is one
mechanism rather than two. Say in the docs that this is not the M6 egress proxy
— that is a process-level control and this is an API-level one. Do not imply
otherwise.

### `timer` is bounded, and that matters

A timer that fires forever is a leak with a friendly name. Both entry points
return a handle, `timer.at` fires once, `timer.every` fires until cancelled or
until a named maximum number of fires. The registry is bounded and the bound is
a named constant. Timers fire on the loop's thread, never a detached one, so
they cannot touch the VM concurrently with a hook: the loop pumps them once per
step, through the same per-call budget a hook gets. A timer that is never pumped
is an inert API, which is why the pump is wired to the loop rather than left to
the caller.

### `/reload` and quarantine

`/reload` builds a fresh `lua_State` for the extension and re-runs its entry
point. The semantics that must be documented rather than discovered: closures
from the old VM are dead, any tool the extension registered is re-registered,
and a tool that is mid-dispatch is **not** torn down underneath the caller —
deferred until the dispatch completes, which is the convention
`tool.unregister` already documents.

Quarantine: an extension whose error count crosses a threshold stops being
called, is reported by name, and does not take the session down. `19`'s
disable-all exit criterion applies — with every first-party extension disabled
the agent must still work, just with less capability.

### The declaration file is a contract, in both directions

`extensions/mcode.d.luau` currently declares entry points that do not exist.
After this slice it must be exact. Where an entry is deliberately absent
(`spawn`), it is **removed from the declaration** rather than left as an
aspiration, because a declared-and-absent name is the failure this slice
exists to fix.

## Files

| Path | Owner | Change |
|---|---|---|
| `src/mcode/ext/api_fs.{hxx,cxx}` | B | `fs.read`, `fs.write`, permission-gated |
| `src/mcode/ext/api_cfg.{hxx,cxx}` | B | `cfg.get`, scoped and read-only |
| `src/mcode/ext/api_cmd.{hxx,cxx}` | B | `cmd.register`, `cmd.handler` |
| `src/mcode/ext/api_timer.{hxx,cxx}` | B | `timer.at`, `timer.every`, bounded registry |
| `src/mcode/ext/api_session.{hxx,cxx}` | B | `session.snapshot`, `session.fork` |
| `src/mcode/ext/api_net.{hxx,cxx}` | B | `net.get`, `net.search`, host allowlist |
| `src/mcode/ext/api_context.{hxx,cxx}` | B | `context.add_instructions` |
| `src/mcode/ext/api.{hxx,cxx}` | B | `ENTRIES` rows, `install_request` fields, `skill.register` |
| `src/mcode/ext/loader.{hxx,cxx}` | B | the new collaborators; `/reload`; quarantine |
| `src/mcode/ext/quarantine.{hxx,cxx}` | B | error counting and the threshold |
| `extensions/mcode.d.luau` | B | exact, in both directions |
| `tests/test_api_surface.cxx` | B | **the declared-vs-implemented test** |
| `tests/test_api_fs_cfg.cxx` | B | |
| `tests/test_api_cmd_timer.cxx` | B | |
| `tests/test_api_net_session.cxx` | B | |
| `tests/test_ext_reload.cxx` | B | reload and quarantine |

## Steps

1. **The declared-vs-implemented test.** It fails on 13 entries; that is the
   worklist.
2. Permission plumbing: the manifest check and the `nil, err` convention, in
   one place, so every entry point below uses the same gate.
3. `fs` — routed through the existing workspace and permission engine.
4. `cfg`, `cmd`.
5. `timer`, with the bound and the loop-thread delivery.
6. `session`, `context.add_instructions`.
7. `net`, with the host allowlist.
8. `skill.register`.
9. `/reload` and quarantine.
10. Reconcile `mcode.d.luau` exactly; confirm `luau-analyze` is clean.

## Acceptance

- **The declared-vs-implemented test passes in both directions**, and would fail
  if either side drifts.
- Every gated entry returns `nil, err` when the manifest lacks the permission,
  and works when it has it. Asserted per entry point.
- `fs.read`/`fs.write` respect the workspace boundary, the protected-path deny,
  and the permission engine — asserted to agree with the `read`/`write` tools on
  the same paths, which is the check that catches a second implementation
  drifting.
- An extension that registers a command has it reachable by name; `cmd.handler`
  resolves it.
- `timer.every` stops at its bound and after cancel; a timer cannot fire after
  the VM is gone.
- `net.get` to a host outside the manifest's declaration is refused; inside it
  succeeds against a loopback server.
- `/reload` re-registers the extension's tools and leaves no stale closure
  callable.
- An extension crossing the error threshold is quarantined, named, and the
  session continues — the M5 exit criterion.
- With every first-party extension disabled the agent still works: the
  disable-all test from `23`.
- `luau-analyze` is clean against the reconciled definition file.

## Traps

- **Declaring without implementing.** The whole point of this slice. The
  both-directions test is the guard; write it first.
- **A second path-resolution implementation in `fs`.** Route through
  `workspace` and `perm`. A drift here writes outside the boundary.
- **A permission checked against a global flag.** It must be the extension's own
  manifest, or one extension's grant becomes every extension's grant.
- **A raise where `nil, err` is the convention.** Extensions can handle the
  second; the first kills the extension.
- **An unbounded timer.** A leak with a friendly name.
- **A timer on a detached thread.** It must fire on the loop's thread or it
  races the VM.
- **Tearing down a tool mid-dispatch on reload.** Defer, as `tool.unregister`
  already does.
- **Calling `net` a sandbox.** It is an API-level host policy. The process-level
  egress control is M6 and slice C; do not let the docs blur the two.
- **Assuming `api.cxx`'s helpers are reachable.** They live in
  `api_internal.{hxx,cxx}` — include it rather than adding a copy.

## Sources

- `docs/18-lua-api.md` — the frozen surface, the amendment precedent, the capability model
- `docs/19-extensions.md` — loader, lifecycle, error containment, quarantine, the disable-all test
- `docs/17-lua-runtime.md` — VM lifecycle, reload semantics, JIT policy
- `docs/12-security.md` — manifest permissions, the trust boundary, why a permission is per-extension
- `docs/23-first-party-extensions.md` — the core/extension split and the bundled catalog
- `extensions/mcode.d.luau` — the declarations this slice must make true
- `src/mcode/ext/api.cxx` — the `ENTRIES` table and the existing 13
