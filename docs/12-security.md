# Security Model

> TL;DR: Two independent layers — a deterministic permission engine (what the agent *asks* to do) and an OS sandbox (what a running process *can* do) — plus a third: the extension VM's capability boundary, which narrows what a loaded extension can reach. The third is **not an OS sandbox** and is never called one in user-facing text.

## Threat model

| # | Threat | Attacker | Severity |
|---|---|---|---|
| T1 | Malicious extension the user installed | Extension author | Full user-level compromise under the trusted tier |
| T2 | Prompt-injected agent writes a malicious extension | Injected instructions | Persistence across sessions — boot-level persistence |
| T3 | **Malicious repo ships `.mcode/extensions/`** | Repo author | One `git clone` + one run = code execution |
| T4 | Engine bugs (Luau VM memory-safety defects) | — | Not mitigated by the capability boundary — the residual risk of any in-process VM |
| T5 | Prompt injection via repo content, web, tool output | Content author | Exfiltration, destructive actions |
| T6 | Malicious MCP server | Server operator | Tool poisoning, confused deputy |

T3 is the one that distinguishes a scripting harness from a plugin-less one, and it has a direct precedent: Neovim's `exrc` problem, which they solved with a persisted trust database and then hardened after a one-key "allow" footgun.

## Layer 1 — Permission engine

### Workspace boundary (defines every rule below)

Every rule in this section keys off "inside workspace", so the boundary is defined first and once:

| Aspect | Definition |
|---|---|
| Root | The **git root** of the cwd if one exists, else the cwd. Detected once at session start and recorded in the session log |
| Extent | The root plus any `--add-dir` paths, resolved to canonical real paths at startup |
| Symlinks | Resolved before comparison. A symlink that escapes the root is **outside**, regardless of where it is written |
| Multi-root | Supported via `--add-dir`; each root gets the same rules. No nested-root precedence |
| Path comparison | Canonical (realpath) + case-folded on Windows/macOS. `..` segments resolved before the check. UNC paths (`\\server\share`) are **always outside** and always prompt (`12` §Traps) |
| Outside the workspace | Reads: `ask`. Writes: `ask`, never auto-persisted. Deletes: `deny` |
| Temp | The OS temp dir is writable without prompting; it is never a read source for project content |

When no git root and no explicit root exist (e.g. `/` or a home directory), mcode **refuses to start in write mode** and says why. A workspace boundary that resolves to the whole filesystem is not a boundary.

### Decision table

Decides *before* a tool call. Rule evaluation: `deny` → `ask` → `allow`, first match wins, from four scopes (managed > user > project > session).

| Action | Default | Persist "don't ask"? | Sandbox? |
|---|---|---|---|
| Read file inside workspace | auto | — | n/a |
| Read outside workspace | ask | session | n/a |
| Read `.env`, `.git/`, keys, `id_rsa*`, `.aws/`, `*.pem` | deny | — | sandbox denyRead |
| Edit/create inside workspace | ask | session | sandboxed |
| Edit outside workspace | ask (never auto) | no | requires unsandboxed approval |
| Write `.mcode/`, `.git/` internals | deny (project cannot override) | — | sandbox denyWrite |
| Shell: read-only builtins (`ls cat grep git status`) | auto inside workspace | — | sandboxed |
| Shell: any write/exec | ask | per exact parsed argv | sandboxed |
| Shell: network (`curl`, `git push`, `npm install`) | ask | per exact argv | sandboxed + egress policy |
| Unparsable/compound command | ask (never auto-allow) | no | sandboxed |
| Package install | ask + show lifecycle scripts | per project | sandboxed, network allowlist |
| Web fetch / search | ask | per domain, per project | through egress proxy |
| MCP tool call | ask per server trust tier | per tool, per project | server process sandboxed |
| MCP server connect | ask, full tool-list diff shown | per server | server sandboxed |
| Git commit | ask | session | sandboxed |
| Git push (force: deny unless flagged) | ask | no | sandboxed |
| `rm`/`rmdir` in workspace | ask | no | sandboxed |
| Delete outside workspace, `sudo`, registry | deny | — | — |
| **Extension load** | **ask + hash-pinned trust grant** | per file, hash-verified each load | sandboxed thread in-process; untrusted tier would be a subprocess |
| **`mcode.spawn` / `mcode.fs.*`** | checked against the manifest's declared permissions | per extension | see Layer 3 |
| YOLO | explicit flag + typed confirmation per session | no | still sandboxed unless `--no-sandbox` (double flag) |

**How the two gate inputs compose.** A permission decision is:

```
allow(tool) = session_policy(tool.class, path_or_argv) ∩ extension_manifest(caller)
```

The session policy keys on **class + path/argv** (`03`), never on tool identity, so a built-in and an extension tool with the same class are treated identically. The extension manifest then **further narrows** it — it can never widen. When they conflict, the **more restrictive wins**: an extension declaring `fs_write` still prompts wherever the session policy prompts, and is denied wherever the session policy denies. An extension with no declared permission for a class is denied that class outright.

This is what makes first-party and third-party extensions equivalent from the permission engine's point of view: same classes, same composition, different provenance (`23`).

Mechanics:

- **Match on parsed argv, not raw strings.** Tokenize; split compound commands; strip known wrappers (`timeout`, `nice`, `env VAR=…`); ask on anything with command substitution, process substitution, or unparseable syntax. Deny rules match every subcommand including inside subshells.
- **No allow-by-prefix for exec-capable runners** (`xargs`, `find -exec`, `docker exec`, `sh -c`).
- **"Don't ask again" persists the parsed argv form**, never a wildcard prefix.
- **Project-sourced config can never widen** — it may add `ask`/`deny` only. `allow` and sandbox-widening keys are honored only from user/managed scope.
- **Break-glass** prints a red banner, requires typing a confirmation word, and is disabled entirely when a managed config exists.

## Layer 2 — OS sandbox

| OS | Mechanism | Strength |
|---|---|---|
| Windows | **Tier 1**: restricted token + Job Object + ACL write boundary (`CreateRestrictedToken`, `CreateJobObjectW` kill-on-close + UI limits). **Tier 2**: AppContainer (`CreateAppContainerProfile`, `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES`). Network deny via WFP scoped to the sandbox identity | Tier 1 needs no admin. Tier 2 AppContainer is default-deny on network and **special-cases loopback**, which collides with the egress proxy. `Experimental_CreateProcessInSandbox` is **not a path** — still experimental and its policy fields silently no-op without AppContainer. Details: `24` |
| Linux | **Landlock primary** (`landlock_create_ruleset`/`add_rule`/`restrict_self`) + seccomp; `bubblewrap` optional enhancer | **Runtime ABI detection is mandatory** — RHEL 9.6 is ABI 5 on a 5.14 kernel. bwrap is broken out-of-the-box on stock Ubuntu 24.04 (AppArmor userns restriction) and setuid mode was removed. Landlock gives ports, not hostnames, and pre-opened fds bypass rules. Details: `24` |
| macOS | **Seatbelt**, entered via `sandbox_init_with_parameters()` (in-process, no exec wrapper, no stderr warning) with `(deny default)` + `(import "system.sb")`. `sandbox-exec` is the documented-but-deprecated fallback | `sandbox-exec` is DEPRECATED (2017) and warns on every run; **Apple has published no replacement** for CLI process sandboxing. `(allow default)` profiles are structurally escapable — deny-default is mandatory. Details: `24` |
| Paranoid | Docker/Podman or microVM with the workspace bind-mounted | Strongest; also the only option where native sandboxing is unavailable. Note Windows Sandbox CLI is *not* programmatic (`wsb exec` has no I/O capture) |

**Network egress is the actual anti-exfiltration control.** A local loopback proxy with a domain allowlist; sandboxed processes get `HTTP(S)_PROXY` and direct egress is blocked at the OS layer. This is the single highest-value component and the one that breaks the lethal trifecta (private data + untrusted content + external communication).

**Secrets hygiene:** deny-read list enforced in both the sandbox and the file tools; env scrubbing for spawned processes (pass a known-safe minimal env, never inherit wholesale); redact secret-shaped strings from logs and transcripts.

## Layer 3 — Extension VM boundary

**The extension VM is a capability boundary, not an OS sandbox.** It runs in the agent process, in the same address space, with the same privileges. It narrows the question from *"an extension can do anything the user can"* to *"an extension can do only what the host API exposes."* It does not survive a VM engine bug, and upstream does not claim it is formally proven. **Never describe it as a sandbox in user-facing text.**

`17` owns why the VM is Luau and `27` the measurements. This section owns what the boundary does and does not contain.

### What it contains

The removals are structural, not a library denylist, and the VM flag that makes globals readonly is unreachable from script. Verified against Luau `c0e346ed`:

| Property | Mechanism | Evidence |
|---|---|---|
| No filesystem, no process execution, no native module loading | `io.` and `package.` absent; `os.` reduced to `clock`/`date`/`difftime`/`time` | `probe_io_reachable=0`, `probe_os_execute_reachable=0` |
| No host reflection | `debug.` reduced to `traceback`/`info`; `dofile`/`loadfile`/`load`/`loadstring`/`collectgarbage`/`string.dump` absent | 10 absence probes, all passing |
| No C-API write bypass | `readonly` is checked on **every** write path (`lua_setfield`, `lua_rawset`, `lua_rawsetfield`, `lua_rawseti`), so the host cannot write past it either | source-verified; `rawset`/`setmetatable` probes refused |
| No bytecode | `loadstring` rejects bytecode; `string.dump`/`load` absent | `probe_loadstring_reachable=0` |
| Globals cannot be monkey-patched | `_G`, every library table, and the string metatable are readonly via a VM-internal flag | `rawset`/`setmetatable` on `_G`, `string`, and `mcode` all refused |
| No finalizer reentrancy or use-after-finalize | `__gc` does not exist; host-only destructors run via `lua_newuserdatadtor` before the block is freed | API inspection |
| Runaway scripts stop | `lua_callbacks()->interrupt` | source-verified below |
| Memory is bounded and attributable | custom `lua_Alloc`; `lua_setmemcat` categories | API inspection |

**Measured: 25 escape probes, 0 escapes** (`mcode` smoke test, `src/main.cxx`). Probes, not proofs.

### What is *not* removed

Three Lua 5.1 base functions survive, verified present: **`newproxy`**, **`setfenv`**, **`getfenv`**. They are not escalations — `newproxy` creates a tagged userdata with no host reach, and `setfenv`/`getfenv` operate only on the caller's own environment — but a claim that the base library is stripped would be false, so they are asserted-present in the smoke test rather than assumed gone.

`luaL_sandboxthread` gives each extension a **writable globals proxy** whose metatable's `__index` points at the frozen host globals. Reads fall through to the host surface; writes land in the extension's own namespace. An extension can therefore shadow a host name (e.g. `mcode`) for itself and nobody else. Self-harm, not escalation — but it means **the host must never read its API surface back out of an extension's globals.**

### The interrupt guarantee, precisely

Upstream documents the interrupt as firing "at any function call or at any loop iteration". **Verified from `lvmexecute.cpp@c0e346ed`:** `VM_INTERRUPT()` appears at exactly eight opcodes — `LOP_CALL`, `LOP_CALLFB`, `LOP_FASTPCALL`, `LOP_RETURN`, `LOP_FORNLOOP`, `LOP_FORGLOOP`, `LOP_JUMPBACK`, `LOP_JUMPX`.

Both directions matter:

- **Any loop is interruptible.** `while true do end` compiles to `JUMPBACK`; `for` to `FORNLOOP`/`FORGLOOP`; recursion to `CALL`. Strictly better than LuaJIT, where `LUA_MASKCOUNT` does not fire under JIT at all.
- **A long straight-line basic block is not.** A function body with a million statements and no call, loop, or return runs to completion before the next safepoint. The interrupt is not per-instruction, so it **cannot be used for instruction counting** — CPU accounting counts safepoints, not work.

The interrupt is also the only callback safe to set from another thread (`lua.h`: "interrupt is safe to set from an arbitrary thread but all other callbacks [are not]"), which is what makes a watchdog possible.

**The kill switch is only as good as the shortest unbounded host call.** The interrupt cannot preempt *inside* a C++ host function, so every function the host exposes must be bounded in time.

### Trust tiers

| Capability | Untrusted (repo-shipped, untrusted) | Semi-trusted (user-granted) | Trusted (user-installed) |
|---|---|---|---|
| Runs in | **separate process**, own VM, Job Object / rlimits | agent process, own sandboxed thread | agent process, own sandboxed thread |
| `ffi` | does not exist | does not exist | does not exist |
| `io` / `package` / `os.execute` / full `debug` / `loadstring` | absent — host capability API only | absent | absent |
| Globals readonly | yes | yes | yes |
| Bytecode | rejected | rejected | allowed, **signed only** |
| Budget | OS wall-clock + kill | interrupt + host watchdog | interrupt + watchdog |
| Memory ceiling | OS Job Object / rlimit | `lua_Alloc` ceiling | ceiling, advisory |
| Network | deny by default, per-call prompt | allowlist | full |
| Error containment | process crash = that extension only | `lua_pcall` per hook | `lua_pcall` per hook |
| Hot reload | restart the extension process | new sandboxed thread | new sandboxed thread |
| Trust grant | prompt + hash-pinned trust DB; view-then-trust, no one-key allow | this tier *is* the grant | install action = grant |

**The boundary is identical across all three tiers** — that is what choosing Luau bought. What differs is the *host capability API* handed to the extension and, for untrusted, the process boundary on top.

**v1 ships the trusted and semi-trusted tiers.** The untrusted tier stays a documented non-goal; `19` keeps the extension-host seam so it can be added without a redesign.

### Repo-shipped extensions (T3)

A cloned repository must never auto-execute code.

1. **Never auto-load repo-local extensions.** `.mcode/extensions/` is inert until the user grants trust. Once granted, a project extension runs at the **semi-trusted tier**: in-process, own sandboxed thread, same boundary as a user-installed extension — not the out-of-process tier, which v1 does not ship.
2. The grant is **per-file and hash-pinned**: show what will load, hash it at grant time, verify at every load. (Neovim 0.12 hardened exactly this after their `:trust` TOCTOU note.)
3. Reject bytecode, symlinks, and absolute-path escapes in manifests.
4. `--no-extensions` exists and is honored for headless/CI runs.

Note T2 (prompt-injected agent writes an extension): the agent has disk write access by design, so it can drop a file into the extensions directory. Mitigations are the trust gate above plus keeping the *user* extension directory (`~/.config/mcode/extensions/`) outside the workspace so the permission engine's workspace-write rules do not cover it.

### Hot reload and hang handling

Materially better than the LuaJIT-era design, because `lua_resetthread` exists:

- **A sandboxed thread can be truly reset.** `lua_resetthread` closes upvalues, clears call frames and thread state, and clears the stack — a genuinely clean thread, not a re-`require` over stale closures and live JIT traces.
- **The VM's global state still persists.** Reset does not unload code reachable from the shared global state (interned strings, readonly library tables). Truly unloading an extension means dropping its thread *and* unregistering anything it registered with the host.
- **Trap: `lua_resetthread` sets `L->finalizers = nullptr` without running them** (`lstate.cpp:170`). Host userdata destructors still queued for that thread are silently skipped and the resource leaks. Drain finalizers before reset.
- **Errors are containable** via `lua_pcall` per hook. **Hangs** via the interrupt plus a host watchdog. **OOM** via the allocator ceiling, since all VM memory routes through `lua_Alloc` — this is where Luau is materially better than LuaJIT, whose FFI allocations bypassed GC accounting entirely.
## What sandboxing does NOT protect against

Explicit, so nobody over-trusts it:

1. **Prompt injection itself** — the model can still be manipulated; sandboxing bounds what the manipulation can *do*.
2. **Exfiltration through allowlisted channels** — anything on the egress allowlist can carry data out in URLs, issue bodies, or package names. An allowlist is not DLP.
3. **Exfiltration via the model API** — repo content sent to the provider is leaving the machine by design.
4. **Writes inside the workspace** — the sandbox permits them by design.
5. **Supply-chain compromise of installed dependencies** — `npm install` runs in the sandbox; credential theft is stopped by env scrubbing and the deny-read list, not by FS isolation.
6. **Credential *use*, not theft** — if the agent may run `git push`, injected instructions can direct legitimate credentialed actions.
7. **User-approved actions** — approving a destructive command defeats everything.
8. **Sandbox escape via OS/kernel 0-days** and via legacy configurations.
9. **Host-side helpers crossing the boundary** — our own file-write and fetch implementations must enforce policy themselves.
10. **Social engineering of the approval prompt** — the human is the last and most fallible line.
11. **A VM memory-safety defect.** The boundary is a property of the VM's implementation, not a proof. A defect in Luau is a full escape from the agent process — the same residual risk any in-process VM carries. This is the reason the untrusted tier stays a documented non-goal (`19`).
12. **Unsigned bytecode.** The VM trusts its compiler. Bytecode skips the parser and the sandbox-installation step, so it is accepted only from signed sources.
13. **Host functions the extension calls.** Every capability is a host C++ function. Its own bugs — a path check that forgets symlinks, a spawn that does not scrub the environment — are outside the VM boundary entirely. The boundary narrows *reach*, not *correctness*.
14. **CPU exhaustion in a long straight-line block.** The interrupt fires at eight opcodes (`12` §Layer 3), not per instruction, so a call-free loop body still yields and is killed, but a huge basic block without a call, branch, or return runs to completion first. Per-extension CPU accounting counts safepoints, not work.
15. **C-stack exhaustion.** Deep non-tail recursion hits the host stack; `lua_checkstack`/`LUAI_MAXCSTACK` bound it, but the failure mode is an abort, not a catchable error.
16. **In-process data theft from other extensions.** Extensions share one VM global state; readonly tables stop *mutation*, not *reads* of anything the host put there. Never place a secret in a global.
17. **Host state reachable through the capability API.** Anything the host exposes — a tool that reads arbitrary paths, a spawn that inherits env — is a capability the extension legitimately holds. Taint labels shape behavior; they do not confine code.
18. **TOCTOU on trust grants** — hash at grant *and* at load.
19. **Accidental global-state corruption** — readonly globals prevent most of it; the rest is hygiene, not security.

## Traps

- **Calling the extension boundary "a sandbox."** It is an in-process capability boundary, not an OS sandbox, and upstream does not claim it is formally proven. Say "capability boundary" or do not ship the claim.
- **Prefix rules as security.** Documented-bypassable; treat the rule engine as UX and the sandbox as the boundary.
- **Auto-allow without a sandbox.** Auto-approval is safe *because* it is sandbox-scoped.
- **Command-substitution blindness.** `echo $(git clean -f)` hides a destructive subcommand inside a read-looking one.
- **UNC paths on Windows.** `\\server\share` can leak NTLM credentials; prompt unconditionally.
- **Inheriting the environment.** Spawned shells inherit tokens and cloud credentials. Scrub aggressively — this, not FS isolation, is what blunts postinstall-style attacks.
- **`.gitignore` is not redaction.** It controls git tracking, not what the agent reads or sends.
- **Job objects are not a security boundary** — resource limits and lifetime only; pair with a token or AppContainer.
- **Rug pulls.** Re-fetching tool descriptions mid-session silently changes what the model sees; hash-pin.
- **Trusting MCP annotations.** `readOnlyHint: true` is a server claim, not a fact.
- **Docker socket in a container sandbox = root equivalent.** Never default to it.
- **Loading repo-local extensions on first run.** The single highest-severity mistake this design can make.

## Open questions

- Windows network deny for the restricted-token tier needs admin setup for WFP rules. Is an env-proxy fallback honest enough to document, or must Windows Tier 1 declare "network isolation best-effort"?
- Should MCP servers reuse the shell sandbox profile or a stricter one (no workspace write)?
- Do we ever build the untrusted tier (subprocess extension host), or does the trusted-only model hold indefinitely?
- Is hash-pinning per file the right grant granularity, or per directory with a manifest hash?
- Seatbelt profiles: a small static set versus per-command generation.

## Sources

- https://luau.org/sandbox/ — library removals, bytecode rejection, readonly globals, `__gc` removal, interrupt, memory limits, upstream caveat
- https://github.com/luau-lang/luau/blob/master/VM/src/lvmexecute.cpp — `VM_INTERRUPT()` at exactly 8 opcodes (source-verified)
- https://github.com/luau-lang/luau/blob/master/VM/src/lstate.cpp — `lua_resetthread` clears call frames, upvalues, stack; skips queued finalizers
- `docs/27-a1-vm-spike.md` — the conformance probes and measurements
- https://github.com/LuaJIT/LuaJIT/issues/779 — `LUA_MASKCOUNT` does not fire under JIT
- https://github.com/Kong/kong/blob/master/kong.conf.default — `untrusted_lua` tiers, sandbox warning
- https://github.com/kong/kong-lua-sandbox — whitelist env, instruction quota
- https://redis.io/docs/latest/develop/programmability/ — sandbox scope, busy-reply threshold, SCRIPT KILL
- https://neovim.io/doc/user/lua/ — `vim.secure`, trust DB, TOCTOU note
- https://raw.githubusercontent.com/mpv-player/mpv/master/DOCS/man/lua.rst — thread-per-script, endless-loop behavior
- https://wezterm.org/config/files.html — config re-evaluation semantics
- https://www.wireshark.org/docs/wsdg_html_chunked/wsluarm.html — superuser script guard
- https://openresty.org/ — `lua_code_cache off` true-unload cost
- https://invariantlabs.ai/blog/mcp-security-notification-tool-poisoning-attacks — tool poisoning, rug pulls
- https://arxiv.org/pdf/2506.02040 — MCP registry acceptance of malicious servers
- https://simonwillison.net/2025/Jun/16/the-lethal-trifecta/ — trifecta model
- https://learn.microsoft.com/en-us/windows/win32/secauthz/restricted-tokens — `CreateRestrictedToken`
- https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer — AppContainer launch
- https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects — Job Object limits
- https://man7.org/linux/man-pages/man7/landlock.7.html — Landlock ABI versions
- https://google-gemini.github.io/gemini-cli/docs/cli/sandbox.html — Seatbelt profiles
