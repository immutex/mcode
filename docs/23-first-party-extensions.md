# First-Party Extensions

> TL;DR: Everything that is not a core file/exec primitive ships as a first-party Lua extension — network tools, git, GitHub, skills, memory, plan tracking. They are the reference implementations of the extension API, they are the dogfooding test that keeps that API honest, and users can disable any of them.

## Why first-party extensions exist

Three jobs, in order of importance:

1. **They prove the API.** An extension API that no first-party code uses is a guess. Shipping real capability through the same surface users get is the only way to know whether the API is sufficient, ergonomic, and versionable. If `web_search` cannot be written as an extension, the API is wrong — and we find that out at M5, not after a public launch.
2. **They are the examples.** The best documentation for extension authoring is a set of working extensions covering the common shapes: a network tool, a stateful tool, a command, an event hook, a workflow tool composing `bash`.
3. **They are opt-in capability.** Network access, GitHub credentials, and provider-specific search are not universal needs. As extensions, they cost nothing when unused and can be disabled per project.

## The split

Core membership is decided by three tests (`06` §Core tool set): **guaranteed** (the agent cannot recover if it fails to load), **trustworthy** (the harness must trust its result and accounting), **invariant-bearing** (it enforces something extension code cannot be trusted with).

| Capability | Where | Why |
|---|---|---|
| `read`, `write`, `edit`, `glob`, `grep` | **core** | Enforce workspace boundary, read-before-write hashing, anchor uniqueness, size caps |
| `bash` | **core** | Owns the sandbox, permission gate, and PTY |
| `ask_user` | **core** | Silent-failure — its absence makes the agent guess instead of asking |
| `tool_search` | **core** | Bootstrapping — it *is* the tool-loading mechanism |
| `task` | **split** | Spawn primitives (session, budget ledger, isolation, depth) are core; the `task` tool shell is a load-bearing bundled extension |
| `todo` | extension | Session-state wrapper; recitation reads session state directly, so it survives disabling |
| `web_search`, `fetch` | extension | Network; provider-specific; not universally needed |
| `skill_read` | extension | Wraps the C++ skills subsystem |
| `memory_search` | extension | Wraps the C++ memory index |
| `git`, `github` | extension | Workflow tools that were never going to be core |
| `browser`, `docker`, `db`, language-specific | extension | Third-party territory; ship two or three as worked examples |

**No capability is both.** A thing is core or it is an extension; there is no third category and no "core but overridable."

## Shipping set (v1)

| Extension | Tools | Capability required | Notes |
|---|---|---|---|
| `task` | `task` | — | **Load-bearing.** Lua shell over the core spawn primitives. Enabled by default; a load failure is a loud session-start warning, not a silent absence |
| `fs-extra` | `todo` | — | Plan tracking over session state. Smallest useful example |
| `skills` | `skill_read` | skills discovered | Wraps `mcode.skill.*` (`08`) |
| `memory` | `memory_search` | memory non-empty | Wraps `mcode.memory.*` (`09`) |
| `web` | `web_search`, `fetch` | network configured | The canonical network example |
| `git` | `git_status`, `git_diff`, `git_commit` | git repo | Workflow tools composing `bash` |
| `github` | `gh_pr`, `gh_issue`, `gh_review` | `gh` CLI + auth | Real capability, non-trivial surface |

**Enabled by default when their capability is configured**, disabled otherwise. Network tools with no network configured are absent, not broken — a tool that exists only to fail is worse than a tool that is not there.

`task` is the one **load-bearing** bundled extension: if it fails to load, delegation is unavailable. It ships enabled-by-default, warns loudly on load failure, and cannot be silently disabled by project config (a project may disable it explicitly, which is a deliberate user act).

`web` and `git` are the two that must be excellent: they are what a new extension author reads first.

## The network split (the security-relevant case)

`web_search` and `fetch` are extensions, but **the extension is policy-free**. Everything security-relevant stays in C++:

| Concern | Owner | Why |
|---|---|---|
| Permission gate (`ask` per domain) | C++ | An extension cannot be trusted to gate itself (`12`) |
| Egress proxy + domain allowlist | C++ | The actual anti-exfiltration control |
| SSRF filtering (private/link-local ranges, DNS pinning) | C++ | Must not be bypassable |
| Size caps and truncation | C++ | Context budget is a harness concern (`05`) |
| Untrusted-content delimiting | C++ | Tool output is untrusted input by definition (`12`) |
| Rate limiting and backoff | C++ | Shared across extensions |
| Provider API shape, query construction, result formatting | **extension** | Genuinely provider-specific |

The extension calls `mcode.net.get(url, opts)` / `mcode.net.search(query, opts)`. Those API calls run the full gate. An extension cannot reach the network any other way — it has no socket API (`18` §What not to expose) — and the permission check knows **which extension is calling**, validating against that extension's declared manifest permissions.

**The guard must be specific, because generic SSRF defense fails.** Concrete requirements for the C++ side:

| Defense | Why the obvious version fails |
|---|---|
| Deny-list covering **loopback, RFC1918, link-local, CGNAT `100.64.0.0/10`, `192.0.0.0/24`, `198.18.0.0/15`, `240.0.0.0/4`**, and v6 equivalents incl. **v4-mapped** (`::ffff:169.254.169.254`) | `100.64.0.0/10` is CGNAT — Alibaba Cloud parks its instance metadata service at `100.100.100.200` inside it. A list that stops at RFC1918 misses a live IMDS |
| **DNS pinning**: resolve once, then pin `host→addr` into the client's resolve map | Resolving twice lets a zero-TTL record answer public to the check and `169.254.169.254` to the connection |
| **Per-hop redirect revalidation**, redirects disabled in the client | A 302 to an internal address bypasses a check performed only on the original URL |
| **Cross-authority header scrub** on redirect (authorization, cookie, proxy-authorization) | Credentials leak to the redirect target |
| **WHATWG URL parsing**, userinfo stripped | `https://example.com@127.0.0.1/` reads as the host `127.0.0.1` to a correct parser and as `example.com` to a naive one. Parser disagreement is the canonical bypass — reject any URL whose host is not read identically by every parser in play |
| Byte cap enforced **at read**, timeout cap, bounded retries | A size check after download is not a cap |

An allowlist of private hosts is the escape hatch, config-owned and host-enforced.

This is the pattern for every capability-backed extension: **the subsystem is C++, the tool surface is Lua.** The extension supplies ergonomics — request shaping, format conversion, truncation, provider auth — all of it revocable and replaceable. The harness supplies guarantees.

## Reference implementations

Each shipping extension is written to demonstrate one shape:

| Extension | Demonstrates |
|---|---|
| `fs-extra` | Minimal tool registration; session state; the smallest complete extension |
| `web` | Network via gated API; config reading; async result handling |
| `git` | Composing `bash`; a workflow tool rather than a CRUD mirror; permission classes |
| `github` | Credentialed external service; structured output; error mapping |
| `skills` | Wrapping a C++ subsystem; a tool whose availability depends on discovery |
| `memory` | Query-shaped tool; result formatting under a budget |

Two rules for first-party extensions, same as any other:

- **They use only the public `mcode` API.** If a first-party extension needs a private escape hatch, that is a bug in the API, not a reason for a back door. There is no `mcode._internal` for first-party code.
- **They are versioned with the harness** and pin an `api_version` exactly like a third-party extension. No privileged loading path.

## Failure and disablement

| Situation | Behavior |
|---|---|
| A first-party extension fails to load | Log, skip, session continues. The agent simply lacks that capability |
| A first-party extension is disabled by the user | Same as failing to load, but silent and intentional |
| A tool it provides is referenced by the system prompt | The prompt section is omitted (`21`) |
| A core tool is missing | Cannot happen — core tools are compiled in |

**Only `task` is load-bearing**, and it is handled explicitly above. Everything else can vanish with a graceful degradation — the agent does the work inline, or without that capability. That is what makes disablement honest rather than decorative.

## Testing

The fixture suite (`11`) exercises first-party extensions as ordinary extensions, which means:

- The **extension loader** is on the critical path of every eval run — it cannot rot untested.
- A **mock `ToolSource`** still satisfies the core agent tests; first-party extensions are additional sources, not special cases.
- **Disabling all first-party extensions** is a supported test configuration and must produce a working (if less capable) agent. If it does not, something became core by accident.

The last one is the important test: it is the mechanical check that the core/extension line has not drifted.

## Traps

- **Letting a second extension become load-bearing.** `task` is the one deliberate exception, handled explicitly. A second one means a hidden core dependency; move it back to C++ or make its absence graceful.
- **A private API for first-party code.** It rots the public API and makes the examples unrepresentative.
- **Shipping an extension as the only example of a pattern.** Two or three worked examples per shape is the minimum; one is an anecdote.
- **Defaulting network tools on when no network is configured.** A tool that exists only to fail wastes a schema and teaches the model to distrust tools.
- **Assuming a disabled extension's prompt section is harmless.** Dangling tool references are the interference class Arbiter measured (`21`).
- **Treating first-party extensions as trusted for permission purposes.** They get the same manifest-permission checks as third-party code. The difference is provenance, not privilege.
- **Time-dependent tool descriptions.** Computing `os.date()` into a tool description at load makes descriptions non-deterministic — it breaks test reproducibility *and* invalidates the prompt cache every session. Inject the current date through the context block instead; descriptions stay static.
- **Crossing the Lua↔C boundary in hot loops.** Walkers and parsers (glob, grep, ripgrep-class) stay in C++; Lua orchestrates. A first-party extension that reimplements a file walk in Lua is a bug, not a demonstration.
- **Losing attribution.** When a bundled extension misbehaves, the user must be able to tell it was the extension and not the core. Stamp the source on every registry entry and prefix every log line and notification with the extension name (`18`).

## Open questions

- How many first-party extensions ship in v1 before the concept is proven? **Seven** is the current set (shipping table above); the risk is that shipping too many hides API gaps behind familiar code.
- Should first-party extensions be updatable independently of the harness, or pinned to it? Independent updates are the ecosystem model but complicate the `api_version` guarantee.
- Does the `web` extension ship a default search provider, or require configuration? Shipping one implies an opinion and a dependency; requiring config is worse first-run UX.
- Should the `github` extension use the `gh` CLI or call the API directly? The CLI is simpler and inherits auth; the API is more capable and has no external dependency.

## Sources

- https://raw.githubusercontent.com/microsoft/vscode-docs/main/api/references/activation-events.md — built-in features shipped as extensions
- https://raw.githubusercontent.com/microsoft/vscode-docs/main/api/advanced-topics/extension-host.md — extension host boundary
- https://neovim.io/doc/user/lua/ — Lua-implemented built-ins (`vim.lsp`, `vim.treesitter`)
- https://github.com/sst/opencode — per-model prompt variants, tool organization
- https://maki.sh/docs/plugins/ — Lua extension precedent with capability manifests
- https://github.com/kfcafe/imp — Lua extension precedent, blocking hooks
- https://www.anthropic.com/engineering/writing-tools-for-agents — tool authoring, workflow-shaped tools
- https://invariantlabs.ai/blog/mcp-security-notification-tool-poisoning-attacks — why extension-provided tool output is untrusted
- https://arxiv.org/pdf/2506.02040 — MCP registry acceptance of malicious servers
- Docs `06` (core tool set), `12` (permission layers), `18` (API surface), `19` (extension lifecycle), `21` (prompt assembly), `11` (eval suite)
