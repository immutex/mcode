# Workstream C — MCP client (stdio)

> TL;DR: Nothing in this tree speaks MCP. This slice adds a stdio MCP client —
> JSON-RPC over a child process's pipes, `initialize` → `tools/list` →
> `tools/call`, a supervisor that restarts a crashed server — and lands the
> tools in the same registry as everything else under `mcp__<server>__<tool>`.
> **Stdio and tools only.** No HTTP transport, no resources, no prompts, no
> sampling or elicitation.

## Why this slice, and why it is last

MCP is how mcode reaches anything it does not implement: a language server, a
database, a ticketing system. `23`'s split says the *subsystem* is C++ and the
*surface* is Lua, so an extension can declare a server without knowing the
protocol. That is the "keep it simple" story for anything the harness does not
do itself.

It is also the largest slice in the batch and the one whose absence costs least
day to day — `bash` already covers a great deal. **If the batch has to ship in
two stages, this is the stage to defer**, and the plan is written so that
deferring it breaks nothing.

Measured against the tree:

| Claim | Reality |
|---|---|
| MCP subsystem | Does not exist. `src/mcp/` is absent |
| `tool_source::mcp` | **Exists** in `core/registry.hxx`. The seam was reserved; nothing fills it |
| `mcode.mcp.register` | In the frozen `18` table, unimplemented |
| `[mcp]` config section | Not in `CONFIG_SECTIONS`, so a config file using it is **rejected as an unknown section** |
| `proc/process.cxx` | Exists, but is a **one-shot** `run_process`: it closes the child's stdin immediately (`in.close( )` at `process.cxx:148`) and drains both pipes to EOF. MCP needs a long-lived bidirectional session, so this is a **new primitive**, not a reuse |
| `mcode.spawn` (the Lua path) | **Not implemented.** So an MCP client cannot be written as a Lua extension at all, which settles the layering question: the transport must be C++ |

## Scope

**In**

- Newline-delimited JSON-RPC framing over a child process's stdin/stdout
- The `initialize` handshake, version verification, `notifications/initialized`
- `tools/list` with `nextCursor` pagination, and `tools/call`
- Tool registration as `mcp__<server>__<tool>`, `tool_source::mcp`, `owner = "mcp:<server>"`
- A supervisor: spawn, EOF/crash detection, restart with capped backoff, graceful shutdown
- `[mcp]` config section and per-server enable/disable
- Per-request timeouts with an absolute maximum, and cancellation on timeout
- `tools/list` hash-pinning, with a re-approval path when it changes
- Untrusted-content delimiting on every tool result
- A stderr ring buffer per server (never an error signal)
- `mcode.mcp.register` for extension-declared servers

**Out**

- Streamable HTTP and the legacy HTTP+SSE transport. `07` ships stdio first and
  says why: a coding CLI's servers are local, stdio needs no auth and no network
  stack, and process death is detectable via pipe EOF for free
- Resources, prompts, completions, logging
- Sampling, elicitation, roots — the server-initiated surfaces. Advertising a
  capability without a handler is worse than not advertising it
- OAuth, and therefore the whole remote-auth checklist in `07`
- `2026-07-28`'s stateless rewrite. `07:46` records that the installed fleet
  overwhelmingly speaks `2024-11-05`..`2025-06-18` and that the deprecation
  policy gives a 12-month window. The `_meta` choke point below is the hedge
- The `2025-11-25` features `07` leaves optional: tasks, URL-mode elicitation,
  sampling with tools, and OIDC discovery. The target is the **2025-06-18
  core**; a server that requires a 2025-11-25 feature is out of scope
- The `task`/experimental async surface

## Design

### Layering

```
supervisor  ── owns process lifetime, restart policy
   │
 transport  ── interface: send(bytes), on_bytes, on_eof
   │             stdio impl: child process + pipes
   │
   client   ── request/response correlation, handshake, capabilities
   │
  source    ── registers tools into tool_registry as mcp__server__tool
```

**Nothing in `core/agent` includes an MCP header.** `07` is explicit, and it is
the same rule that keeps the loop unaware of Lua. The loop sees a registry entry.

### The one choke point

Every outgoing call goes through a single `call( method, params )` that stamps a
`_meta` map. Today it stamps nothing useful; when `2026-07-28` is adopted it is
where `io.modelcontextprotocol/protocolVersion` and `clientCapabilities` are
injected and the handshake becomes a no-op. **Do not build session state deeper
than the client object**, or that migration becomes a rewrite.

### Framing, and the Windows trap

Newline-delimited JSON on the child's stdin/stdout. **Pipes must be opened in
binary mode.** Text mode translates `\n` to `\r\n` on Windows and the framing
breaks — `07` names this, and it is the kind of defect that only appears on one
platform.

**A non-JSON-RPC line on stdout is skipped, not fatal, and logged.** Real servers
print banners. Treating a banner as a protocol error would make mcode
incompatible with servers that work everywhere else.

### Two ways to configure a server, and the simple one comes first

**The primary path is `config.toml`. No extension, no Lua, no code.**

```toml
[mcp.servers.filesystem]
command = "npx"
args = ["-y", "@modelcontextprotocol/server-filesystem", "/path/to/allow"]
enabled = true
```

That is the "keep it stupidly simple" path, and it is the one the docs should
lead with: a user who wants one more capability edits four lines and restarts.

The second path is `mcode.mcp.register` (Phase 0 P3), for an extension that
wants to declare a server as part of a larger setup — a language server that
ships with the extension's own tooling. It is the same data, expressed in Lua.
**Both funnel into one `server_config` struct**, so there is one implementation
of everything after parsing.

An extension declaring a server does **not** bypass consent: the launch command
still requires explicit approval the first time, and the `mcp` manifest
permission gates the call.

### Tool registration

`mcp__<server>__<tool>`, which is stable, collision-free against core tool names
(`__` is not used by any core tool), and legible to the model.

**`tool_class`.** MCP tools are registered as `tool_class::mcp` — a new enumerator
added in Phase 0. They cannot be trusted to declare their own class: `07` is
explicit that `readOnlyHint` is a server *claim*, and the spec requires clients to
treat annotations as untrusted. The class therefore defaults to "ask", and
workstream A's engine decides. A server cannot mark itself read-only to skip the
prompt.

**Schema cost is a budget problem, not a nicety.** `07` measured a 25× spread
across real servers — GitHub at ~136 tokens per tool, Notion at ~715 — and five
common servers at 13% of a 200K window before the first user message. So:

1. A newly configured server is **disabled by default**; enabling it is explicit.
2. On `tools/list`, the client estimates schema tokens and **warns above 8K for
   one server**.
3. The estimate is reported by `mcode ext doctor`-style output so the cost is
   visible before the user enables it.

Lazy loading behind `tool_search` is the real fix (`07` §Token budget), but it
needs the context manager's cache-neutral append path, which does not exist yet.
The toggle and the warning are what is honest to ship now.

### Supervision

- Spawn **never through a shell**. The command line comes from config, and a
  shell would turn a config typo into an injection.
- **EOF on stdout, or the process handle signalling, marks the server failed**
  and rejects every pending call with a transport error. Pending calls are
  **never replayed blind** — a tool call may not be idempotent.
- Restart with backoff 1s/2s/4s/8s/16s, then give up and surface it. On restart:
  re-`initialize`, re-`tools/list`, re-register. **`07` documents that [CC] got
  this wrong** — its stdio servers were excluded from reconnect entirely and
  needed a manual command, while HTTP got backoff. Do not copy that.
- Graceful shutdown: close stdin, wait ≤5s, terminate, wait ≤2s, kill. Children
  must not outlive mcode.
- Per-request timeout (30s list, 120s call, configurable) **plus an absolute
  maximum**. On timeout: send `notifications/cancelled`, stop waiting, ignore a
  late response.

### Untrusted content

A tool description and a tool result are **attacker-controlled input**. `07`
documents a real incident where instructions inside tool descriptions induced an
agent to read SSH keys and other servers' configuration.

- Descriptions and results are wrapped in untrusted-data delimiters before they
  reach the model, never interpolated as instructions.
- `tools/list` results are **hash-pinned**. A server that changes its tool
  definitions after approval is re-prompted — the rug-pull defense.
- The launch command is shown **untruncated** and requires explicit consent.
  A truncated command line widens the gap between what the user approved and
  what runs.

### The test fixture

Tests need a server. **A C++ fixture binary** (`tests/fixtures/mcp_echo.cxx`,
built as its own target) that speaks newline-delimited JSON-RPC and can be told
to: list two tools, echo a call, print a banner before its first response, exit
mid-request, and change its `tools/list` output on demand. Deterministic, no
external runtime, and it exercises the failure paths — which is the whole point,
since `07`'s evidence is that the failure paths are what ship broken.

## Files

| Path | Owner | Change |
|---|---|---|
| `src/mcode/mcp/jsonrpc.{hxx,cxx}` | C | framing, id-keyed dispatch, error codes |
| `src/mcode/mcp/transport.hxx` | C | `send`, `on_message`, `on_eof` |
| `src/mcode/mcp/transport_stdio.{hxx,cxx}` | C | child process, binary pipes, stderr ring |
| `src/mcode/mcp/client.{hxx,cxx}` | C | handshake, `tools/list`, `tools/call`, the `_meta` choke point |
| `src/mcode/mcp/supervisor.{hxx,cxx}` | C | lifetime, restart backoff, shutdown |
| `src/mcode/proc/session.{hxx,cxx}` | C | long-lived child with an **open stdin** and an incremental read loop; `run_process` cannot be reused |
| `src/mcode/mcp/source.{hxx,cxx}` | C | registry registration and teardown |
| `src/mcode/support/config.hxx` | C | adds `[mcp]` to `CONFIG_SECTIONS` **in the same commit as its reader**, never before — a section nothing reads is a silent no-op (`22`) |
| `docs/22-config-and-cli.md` | C | the `[mcp]` section itself, and the `[telemetry]` example fix below. Phase 0 adds the section *name* to the config list; C documents what goes in it |

**A pre-existing divergence found while planning this, fixed in the same commit
because C already touches both files.** `docs/22`'s example config shows a
`[telemetry]` section, but `CONFIG_SECTIONS` in `support/config.hxx` does not
contain it, and an unknown section is a **hard load error**. A user who copies
the documented example gets a config that refuses to load. Either the example
loses the section or the list gains it; the doc's own §Telemetry says telemetry
ships in no version at all, so **the example is what is wrong**.
| `src/mcode/ext/api_mcp.{hxx,cxx}` | C | **creates** these: the handler for `mcp.register` |
| `src/mcode/ext/api.hxx` | C | the method declaration |
| `src/mcode/ext/api.cxx` | C | the `ENTRIES` row. B adds its own to the same table — a one-line conflict integration resolves |
| `tests/fixtures/mcp_echo.cxx` | C | the protocol fixture |
| `tests/CMakeLists.txt` | C | fixture target |
| `src/cli_commands.cxx` | **integration** | construct the supervisor, connect servers |

## Steps

1. JSON-RPC framing and dispatch, unit-tested against the fixture's bytes with
   no process involved. Framing bugs are the ones that are impossible to debug
   later.
2. `proc/session`: spawn with a **kept-open** stdin, an incremental stdout read
   loop, and EOF as an event. This is the piece that does not exist, and both
   the transport and any future interactive child process need it.
3. The stdio transport, including binary mode and the banner-skipping rule.
4. The client: handshake, version check, `tools/list` with pagination, and
   **hash-pinning of the list** so a server that changes its tools after
   approval is detected rather than silently re-registered.
5. The supervisor: spawn, EOF detection, restart backoff, graceful shutdown.
6. `source`: registration, `tool_class::mcp`, `owner`, teardown on server death.
7. `tools/call`, with timeout, cancellation and untrusted delimiting.
8. The `[mcp.servers.*]` config section, per-server enable, schema-token
   warning — plus the `docs/22` fix for the `[telemetry]` example, which is a
   one-line doc change in a file this step already touches.
9. `mcode.mcp.register`, which is the same `server_config` from Lua.

## Acceptance

- **A configured server's tools are in the registry and callable.** The fixture
  lists two tools; both appear as `mcp__echo__<name>`; the model calls one and
  the result comes back.
- **A server that crashes is restarted and its tools come back.** Kill the
  fixture mid-run; the supervisor restarts it, re-initializes, re-lists, and a
  subsequent call succeeds. This is the defect `07` documents in [CC].
- **A pending call is failed, not replayed.** Kill the fixture during a call;
  the call fails with a transport error and is not silently retried.
- **A banner on stdout does not corrupt the stream.** The fixture prints a
  non-JSON line before its first response; the handshake still completes.
- **A changed `tools/list` is detected.** The fixture returns different tool
  definitions on the second `list`; the hash mismatch is reported rather than
  silently accepted.
- **A server is not enabled by default**, and enabling one whose schemas exceed
  8K tokens warns with the measured cost.
- **MCP tools are `tool_class::mcp`**, so a server claiming `readOnlyHint` does
  not skip the approval path.

- **A hung server times out and the call is cancelled.** The fixture accepts a
  request and never answers; the call fails at the timeout with
  `notifications/cancelled` sent, and the loop is not blocked. A late response
  arriving afterwards is ignored rather than delivered as a second result.
- **stderr is buffered, never fatal.** The fixture writes to stderr and exits
  non-zero; the output is captured for the status view and does not fail the
  server or the session.
- **`mcode.mcp.register` produces the same server as the config path.** An
  extension declares a server; it appears in the registry exactly as a
  config-declared one does, because both funnel into one `server_config`. The
  `mcp` manifest permission is required, and its absence is a `nil, err`.

Live evidence: connect one real stdio MCP server (a filesystem or git server),
call one of its tools through a real turn, and confirm the result reaches the
model — the same way the last batch found its defects by pointing the harness at
something real instead of at its own stub.

## Traps

- **Text-mode pipes on Windows.** CRLF translation breaks newline framing.
- **Treating stderr as failure.** It is general logging; buffer it, never fail on it.
- **Replaying a pending call after a crash.** A tool call may not be idempotent.
- **Trusting annotations.** `readOnlyHint` is a claim. The class is ours.
- **Truncating the launch command in the consent prompt.** The user must approve
  what actually runs.
- **Not re-listing after a restart.** `07` documents a follow-up bug where
  respawned servers had their tools wrongly deregistered.
- **Building session state deep in the client.** It makes the `2026-07-28`
  migration a rewrite instead of a `_meta` change.
- **Letting schema cost be invisible.** Five servers is 13% of a 200K window.
  Warn, and default new servers off.
- **An MCP tool name colliding with a core tool.** `mcp__` prefixing prevents it;
  the registry already rejects duplicate names, so a collision is a loud failure
  rather than a silent shadow.
- **A server that never responds blocking the loop.** Every request has a
  timeout and an absolute maximum.

## Sources

- `docs/07-mcp.md` — the protocol target, the stdio-first decision, the architecture, the supervision requirements, the token measurements, and the security checklist
- `docs/12-security.md` — MCP rows in the decision table, untrusted annotations, the confused-deputy and rug-pull traps
- `docs/18-lua-api.md` — `mcode.mcp.register` and the `mcp` manifest permission
- `docs/23-first-party-extensions.md` — the subsystem-in-C++ / surface-in-Lua split
- `docs/14-cpp23-stack.md` — yyjson for JSON, the dependency budget
- `src/mcode/core/registry.hxx` — `tool_source::mcp`, already reserved
- `src/mcode/proc/process.cxx` — the existing one-shot spawn, and why a long-lived transport is a sibling rather than a reuse
- https://modelcontextprotocol.io/specification/2025-06-18/basic/transports.md — stdio framing
- https://github.com/anthropics/claude-code/issues/43177 — the stdio reconnect gap to avoid
- https://invariantlabs.ai/blog/mcp-security-notification-tool-poisoning-attacks — tool poisoning, why descriptions are untrusted
- https://github.com/zhang-liz/mcp-token-benchmark — the 25× schema-cost spread
