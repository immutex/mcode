# Security Model

> TL;DR: Two independent layers — a deterministic permission engine (what the agent *asks* to do) and an OS sandbox (what a running process *can* do) — plus a third: a Lua trust boundary that admits the extension VM is not a sandbox at all and isolates untrusted extensions by process.

## Threat model

| # | Threat | Attacker | Severity |
|---|---|---|---|
| T1 | Malicious extension the user installed | Extension author | Full user-level compromise under the trusted tier |
| T2 | Prompt-injected agent writes a malicious extension | Injected instructions | Persistence across sessions — boot-level persistence |
| T3 | **Malicious repo ships `.mcode/extensions/`** | Repo author | One `git clone` + one run = code execution |
| T4 | Engine bugs (LuaJIT / Lua CVEs) | — | Not mitigated by any Lua-level sandbox |
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
| **Lua extension load** | **ask + hash-pinned trust grant** | per file, hash-verified each load | trusted tier in-process; untrusted in subprocess |
| **Lua `mcode.spawn` / `mcode.fs.*`** | checked against the manifest's declared permissions | per extension | see Layer 3 |
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

## Layer 3 — Lua trust boundary

**The extension VM is not a security boundary, and must never be described as one.**

This is not a limitation we are choosing; it is a property of LuaJIT, stated by its author:

- LuaJIT FAQ: *"In general, the only promising approach is to sandbox Lua code at the process level and not the VM level."* Also: *"loading untrusted bytecode is not safe! It's trivial to crash the Lua or LuaJIT VM with maliciously crafted bytecode… there's no bytecode verification on purpose."*
- FFI semantics doc: *"the FFI library is **not safe for use by untrusted Lua code**. If you're sandboxing untrusted Lua code, you definitely don't want to give this code access to the FFI library or to *any* cdata object."*
- Kong ships a LuaJIT sandbox and still documents it as *"protection against trivial attackers or unintentional modification of the Kong global environment."*

### Why `ffi` ends the argument

`ffi.C` binds to the process's global symbol namespace — on Windows that includes `kernel32.dll`; on POSIX libc, libm, libdl. `ffi.cdef` declares any C prototype at runtime, `ffi.load` opens any DLL/SO, and `ffi.cast` plus `VirtualProtect`/`mprotect` gives arbitrary native code execution from pure Lua.

So `ffi.cdef[["int system(const char*);"]] ffi.C.system("…")` is a one-liner. **With `ffi` reachable, Lua-level sandboxing is impossible. Period.**

Worse, hiding `ffi` is not sufficient: a bytecode chunk containing a cdata literal **re-initializes the FFI library on load even if `ffi` was never registered** — a documented full escape from a no-stdlib, no-ffi sandbox to native code execution.

And the empirical record agrees. Luanti shipped a mod sandbox and had to fix CVE-2026-40959: *"attackers [can] escape the Lua sandbox through crafted mods when LuaJIT is used"* → RCE. Factorio's Lua bytecode verifier was bypassed repeatedly until upstream removed the verifier entirely, saying it *"seems useless to make a promise that we can't seem to deliver."* Redis's CVE-2022-0543 was a packaging slip that left `package.loadlib` reachable, yielding `os.execute` at CVSS 10.0.

### Trust tiers

| Capability | Untrusted (repo-shipped / not yet trusted) | Semi-trusted (user-granted, fenced) | Trusted (user-installed) |
|---|---|---|---|
| Runs in | **separate process**, own LuaJIT VM, Job Object / rlimits | agent process, own `lua_State`, `jit.off()` | agent process, JIT on |
| `ffi` | never reachable; bytecode disabled | no | yes |
| `io` / `os` / `package` / `debug` / `load` | no — host-injected capability API only | host wrappers, scoped paths | full stdlib |
| Bytecode loading | rejected (`load(..., "t")`) | rejected | allowed |
| Budget | OS wall-clock + kill | count hook with `jit.off()` (10–20% tax) | cooperative |
| Memory ceiling | OS Job Object / rlimit | allocator or GC polling | none |
| Network | deny by default, per-call prompt | allowlist | full |
| Error containment | process crash = that extension only | `pcall` per hook | `pcall` per hook |
| Hot reload | restart the extension process | new `lua_State` | `package.loaded[name]=nil` + re-require (stale closures are a known limitation) |
| Trust grant | explicit prompt + persisted **hash-pinned** trust DB; view-then-trust, no one-key allow | this tier *is* the grant | install action = grant |

**v1 ships the trusted and semi-trusted tiers only.** The untrusted tier (out-of-process, for a marketplace) is out of scope; if it ever becomes a goal the answer is a process boundary or Luau — never LuaJIT in-process.

### Repo-shipped extensions (T3)

A cloned repository must never auto-execute code. The rule:

1. **Never auto-load repo-local extensions.** `.mcode/extensions/` in a project is inert until the user grants trust. Once granted, a project extension runs at the **semi-trusted tier**: in-process, in its own `lua_State`, with `jit.off()` and no `ffi` — not the out-of-process untrusted tier, which v1 does not ship (`12` §Layer 3).
2. The grant is **per-file and hash-pinned**: show what will load, hash it at grant time, verify the hash at every load. (Neovim's 0.12 release hardened exactly this after their `:trust` TOCTOU note.)
3. Reject bytecode, symlinks, and absolute-path escapes in manifests.
4. Repo-local extensions run at the **semi-trusted tier** — granted, in-process, `ffi`-free, `jit.off()`, manifest permissions enforced. The out-of-process untrusted tier is a documented non-goal for v1.
5. `--no-extensions` exists and is honored for headless/CI runs.

Note T2 (prompt-injected agent writes an extension): the agent has disk write access by design, so it can drop a file into the extensions directory. Mitigations are the trust gate above plus keeping the *user* extension directory (`~/.config/mcode/extensions/`) outside the workspace so the permission engine's workspace-write rules do not cover it.

### Hot reload and hang handling

- **You cannot unload Lua state.** `package.loaded[name] = nil` + `require` merely re-executes the file; old closures, upvalues, timers, callbacks, JIT traces, and cdata from the previous incarnation stay reachable and keep running.
- True unload requires a **new VM** (OpenResty's `lua_code_cache off` does this per request and pays an order-of-magnitude penalty) or a **process restart**.
- **Errors are containable** via `lua_pcall`/`pcall` boundaries per hook.
- **Infinite loops and OOM are not containable in-process.** Instruction-count hooks do not fire under JIT, and OOM surfaces as `PANIC: unprotected error in call to Lua API`, which terminates the process. Redis accepts this too: it does not kill scripts mid-run, it freezes and offers `SCRIPT KILL` / `SHUTDOWN NOSAVE`.
- Practical consequence: per-extension `lua_State` buys *error* isolation only. Hang and OOM isolation require the extension-host subprocess.

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
11. **`ffi` in any reachable form** — arbitrary memory R/W, arbitrary C calls, native code execution.
12. **Bytecode loading** — no verifier exists by design; escape and VM crash.
13. **LuaJIT/Lua engine bugs** — CVE-2024-25176/77/78 (parser/GC), Redis CVE-2025-49844 (UAF in the parser, CVSS 9.9, 13 years old), CVE-2025-46817/46818.
14. **CPU exhaustion under JIT** — only a process-level kill works.
15. **Memory exhaustion beyond a ceiling that cannot be reliably set** — FFI allocations bypass GC accounting entirely.
16. **C-stack exhaustion and "bad callback" VM PANIC** via FFI callback re-entry — process-fatal, not catchable.
17. **In-process data theft** — anything in the agent process can read agent state, env-var API keys, and other extensions' tables. Taint labels shape behavior; they do not confine code.
18. **TOCTOU on trust grants** — hash at grant *and* at load.
19. **Accidental global-state corruption** — hygiene, not security.

## Traps

- **Calling the Lua sandbox "security."** Every project that tried it documents that it stops only accidents. Say "hygiene" or do not ship it.
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

- https://luajit.org/faq.html — process-level sandboxing stance, bytecode danger
- https://luajit.org/ext_ffi_semantics.html — FFI is not safe for untrusted code; no memory safety
- https://luajit.org/ext_ffi_api.html — `ffi.C` symbol namespaces, `ffi.load`, `ffi.cast`
- https://luajit.org/extensions.html — `load` modes ("t" = source only)
- https://www.corsix.org/content/malicious-luajit-bytecode — full sandbox escape via bytecode cdata literal
- https://github.com/Kong/kong/blob/master/kong.conf.default — `untrusted_lua` tiers, sandbox warning
- https://github.com/kong/kong-lua-sandbox — whitelist env, instruction quota
- https://redis.io/docs/latest/develop/programmability/ — sandbox scope, busy-reply threshold, SCRIPT KILL
- https://www.ubercomp.com/posts/2022-01-20_redis_on_debian_rce — CVE-2022-0543 `package.loadlib` → `os.execute`
- https://memorycorruption.net/posts/rce-lua-factorio/ — bytecode verifier bypass; upstream removing the verifier
- https://github.com/luanti-org/luanti/security/advisories/GHSA-g596-mf82-w8c3 — CVE-2026-40959 LuaJIT sandbox escape
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
