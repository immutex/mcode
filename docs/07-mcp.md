# MCP (Model Context Protocol) Client Design

> TL;DR: Target the 2025-06-18/2025-11-25 protocol core (stdio first, Streamable HTTP second), implement client features (elicitation, sampling, roots) as capability-gated optional surfaces, treat every tool description and tool result as untrusted input, and budget tool schemas aggressively — 50 eager tools can eat 5–35% of a 200K context window before the first user message.

## State of the field

MCP is Anthropic-originated (Nov 2024), now governed by the modelcontextprotocol GitHub org under the Agentic AI Foundation. It is JSON-RPC 2.0 over pluggable transports, with a capability-negotiated lifecycle. Five spec revisions exist; **as of 2026-09 the current revision is `2026-07-28`**.

| Revision | Date | Major changes (verified from official changelogs) |
|---|---|---|
| 2024-11-05 | Nov 2024 | Initial: stdio + HTTP+SSE (two-endpoint: GET SSE stream returns `endpoint` event, POST to that URI), tools/resources/prompts, sampling, roots, logging, ping, cancellation |
| 2025-03-26 | Mar 2025 | OAuth 2.1 authorization framework; **Streamable HTTP replaces HTTP+SSE** (single endpoint, POST + optional GET); JSON-RPC batching added; tool annotations (`readOnlyHint`, `destructiveHint`...); audio content; `completions` capability; `ProgressNotification.message` |
| 2025-06-18 | Jun 2025 | **Batching removed** (added then removed within one cycle); structured tool output (`structuredContent` + `outputSchema`, clients SHOULD validate); resource links in tool results; **elicitation** (server→client structured user input, flat-schema subset); servers classified as OAuth Resource Servers (RFC 9728 metadata); clients MUST implement RFC 8707 resource indicators; `MCP-Protocol-Version` header required on all HTTP requests; `title` fields; `_meta` standardized; lifecycle SHOULDs became MUSTs |
| 2025-11-25 | Nov 2025 | **Experimental tasks** (durable async requests: `working`/`input_required`/`completed`/`failed`/`cancelled`); URL-mode elicitation (browser flow, credentials bypass client); sampling gains `tools`/`toolChoice` (server-side agent loops); OIDC Discovery; OAuth Client ID Metadata Documents (URL-as-client-ID, alternative to DCR); incremental scopes via `WWW-Authenticate`; elicitation enums revised + primitive defaults; icons metadata; invalid `Origin` on Streamable HTTP → MUST 403; SSE polling (server may close connection, `retry` field, resumption always via GET + `Last-Event-ID`); JSON Schema 2020-12 default dialect; stdio stderr clarified for all logging; validation failures should be tool execution errors (isError), not protocol errors |
| 2026-07-28 | Jul 2026 | **Radical stateless rewrite**: removes `initialize`/`initialized` handshake and `Mcp-Session-Id` sessions entirely; every request carries protocol version + client capabilities in `_meta` (`io.modelcontextprotocol/*`); new `server/discover` RPC replaces handshake probing; GET endpoint and `resources/subscribe` replaced by single `subscriptions/listen` POST stream; `ping`, `logging/setLevel`, `notifications/roots/list_changed` removed; tasks moved to extension `io.modelcontextprotocol/tasks`; **MRTR** (Multi Round-Trip Requests) replaces server-initiated requests (roots/list, sampling, elicitation) with `resultType: "input_required"` + client retry with `inputResponses`; SSE resumability/`Last-Event-ID` removed; list results require `ttlMs`/`cacheScope` cache hints; **Roots, Sampling, Logging deprecated**; DCR deprecated for Client ID Metadata Documents; error-code range partitioned (-32020..-32099 spec-reserved) |

Key sources: official changelogs for [2025-03-26](https://modelcontextprotocol.io/specification/2025-03-26/changelog.md), [2025-06-18](https://modelcontextprotocol.io/specification/2025-06-18/changelog.md), [2025-11-25 (GitHub)](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2025-11-25/changelog.mdx), [2026-07-28 (GitHub)](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/changelog.mdx), plus full [transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports.md), [lifecycle](https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle.md), [elicitation](https://modelcontextprotocol.io/specification/2025-06-18/client/elicitation.md), [roots](https://modelcontextprotocol.io/specification/2025-06-18/client/roots.md), [sampling](https://modelcontextprotocol.io/specification/2025-06-18/client/sampling.md), [tools](https://modelcontextprotocol.io/specification/2025-06-18/server/tools.md), [security best practices](https://modelcontextprotocol.io/docs/2025-11-25/tutorials/security/security_best_practices.md) pages — all fetched directly.

C++ ecosystem: no official C++ SDK (SDK tiering covers TS, Python, Go, etc.). Community options: [hkr04/cpp-mcp](https://github.com/hkr04/cpp-mcp) (header-only-ish, stdio + SSE client/server), TinyMCP, IBM TSAR-MCP (zero-dep C/C++ server SDK, edge focus). All are small projects; none advertised full 2025-11-25 client feature coverage at review time. Writing our own thin client is realistic — the protocol core is ~15 RPC methods.

Ecosystem size: official registry (`registry.modelcontextprotocol.io`, preview 2025-09-08) reported **10,000 active servers Dec 2025** ([blog.modelcontextprotocol.io](https://blog.modelcontextprotocol.io/posts/2025-12-09-mcp-joins-agentic-ai-foundation/)); independent Sept 2026 snapshots count **~30–32K active servers** ([mcpindex.ai/stats](https://mcpindex.ai/stats), [devtoolhub.com](https://devtoolhub.com/mcp-registry-by-the-numbers/)). Directories (Smithery, Glama, PulseMCP, mcp.run) are separate from the registry.

## What works / what doesn't

**Transports** (from 2025-11-25 spec text):

| | stdio | Streamable HTTP | HTTP+SSE (legacy) |
|---|---|---|---|
| Lifecycle | subprocess owned by client | independent server, sessions optional (`MCP-Session-Id` header) | two endpoints, `endpoint` event handshake |
| Framing | newline-delimited JSON-RPC, no embedded newlines | POST per message; response = `application/json` OR `text/event-stream`; GET for server-initiated stream | SSE + POST |
| State | none | optional session (404 → MUST re-initialize) | stateful SSE connection |
| Auth | N/A (child process) | OAuth 2.1 since 2025-03-26 | pre-OAuth era |
| Status | recommended, `Clients SHOULD support stdio whenever possible` | current standard for remote | deprecated 2025-03-26; reclassified Deprecated 2026-07-28 |

- stdio shutdown (2025-06-18 lifecycle): close stdin → wait → SIGTERM → SIGKILL. 2026-07-28 stdio spec adds: client SHOULD restart crashed servers promptly; in-flight requests dropped; Windows termination via TerminateProcess/Job Objects.
- Cancellation: `notifications/cancelled` with `requestId`; `initialize` MUST NOT be cancelled; receivers MAY ignore late/unknown cancels; sender SHOULD ignore late responses. Race conditions MUST be tolerated.
- Timeouts: spec says implementations SHOULD timeout all requests, MAY reset clock on progress notifications, SHOULD always enforce a max timeout regardless.
- **Tool schema token bloat is the biggest measured problem.** Benchmark of 9 real servers ([zhang-liz/mcp-token-benchmark](https://github.com/zhang-liz/mcp-token-benchmark)): Notion 24 tools = 17,161 tokens (~715/tool, 97% from `inputSchema`), Firecrawl ~637/tool, GitHub ~136/tool, Slack ~85/tool — a 25× spread. Five common servers = 26,224 tokens = 13.1% of 200K / 20.5% of 128K before any user content. Anthropic (vendor, but engineering blog with methodology) reports 58 tools ≈ 55K tokens, worst observed 134K; its Tool Search Tool cut a 72K eager surface to 8.7K (~85% reduction) via deferred schema loading ([anthropic.com/engineering/advanced-tool-use](https://www.anthropic.com/engineering/advanced-tool-use)).
- **Mitigation landscape** (verified via gateway docs): static allowlist filtering (Docker MCP Gateway `--tools server:tool`, [TBXark/mcp-proxy](https://github.com/TBXark/mcp-proxy) `toolFilter`) reduces proportionally to tools hidden; meta-tool discovery (MetaMCP: 4 tools `mcp_discover/provision/call/execute`, claims ~1,000 schema tokens client-side) reduces ~99% initially but adds an indirection hop and can hurt tool-selection reliability; BM25/semantic lazy loading (MCPProxy) sits between. `mcpm` is a config/manager, not a token optimizer. Practical thresholds from the comparison: <30 tools → allowlists fine; 30–150 → profiles/filtering proxy; 150+ → discovery layer.
- **Claude Code's stdio reconnect gap** ([anthropics/claude-code#43177](https://github.com/anthropics/claude-code/issues/43177)): its client treated stdio servers as permanently failed after disconnect (manual `/mcp` required) while HTTP transports got exponential backoff (1s..16s, 5 attempts). A follow-up issue (#74329) found respawned stdio servers got their tools wrongly deregistered. Lesson: stdio respawn + re-`initialize` + tool re-list must be a first-class automatic path, not an afterthought.
- Elicitation works but is deliberately crippled: flat object schemas, primitives only (string/number/boolean/enum, formats email/uri/date/date-time), three-action response (accept/decline/cancel). Servers MUST NOT request sensitive info. 2026-07-28 replaces it with MRTR.
- 2025-03-26's JSON-RPC batching lasted exactly one revision — evidence that speculative protocol features churn; don't build on them.

## Recommended design for mcode

**Protocol target: implement the 2025-06-18 core + selected 2025-11-25 features.** Do not chase 2026-07-28 yet: it deprecates Roots/Sampling/Logging and rewrites the lifecycle, but the installed server fleet (tens of thousands of servers) overwhelmingly speaks 2024-11-05..2025-06-18, and the deprecation policy gives a 12-month window. Design so the stateless migration is cheap (see below).

**MCP is a `ToolSource`, not agent logic.** It implements the same interface as the built-in and Lua sources (`06` §Tool sources), registers its tools into the shared registry under `mcp__<server>__<tool>`, and the agent cannot tell an MCP tool from a built-in one. Concretely: nothing in `core/agent` may include an MCP header. Extensions may also declare and configure MCP servers through `mcode.mcp(...)` (`18`), which means the MCP subsystem is reachable from Lua without Lua knowing anything about the protocol.

**Architecture: own JSON-RPC + transport layer; no SDK dependency.**

```
src/mcp/
  jsonrpc.hxx      // id-keyed dispatch, error codes, notifications
  transport.hxx    // interface: send(msg), async recv loop
  transport_stdio.cxx  // child process + pipes (overlapped IO on Windows)
  transport_http.cxx   // libcurl: POST + optional SSE GET
  client.hxx       // session state machine, capability registry
  supervisor.cxx   // process lifecycle, restart policy
```

- Dependencies: **yyjson** for JSON (`14`) and libcurl only if HTTP transport needs it; stdio needs nothing beyond the OS. Total added binary weight target < 1.5 MB.
- Wire model: newline-delimited JSON for stdio; each line parsed into a variant of Request/Notification/Response. Request IDs: monotonic u64. Pending-call map: `std::unordered_map<id, std::promise<json>>` with per-request deadline timers.

**Transport decision (the required recommendation):**

| | Verdict | Reasoning |
|---|---|---|
| stdio | **Ship first, full support** | This is a coding CLI: every relevant server (filesystem, git, linters, language servers) runs locally. Zero network stack, zero auth, process death is detectable via pipe EOF (crash detection for free). Spec explicitly says clients SHOULD support stdio whenever possible. |
| Streamable HTTP | **Ship second, minimal profile** | Needed only for remote/hosted servers. Minimal profile: POST-only client, accept both `application/json` and SSE responses, honor `MCP-Session-Id` + `MCP-Protocol-Version` headers, DELETE on shutdown, 404 → re-initialize. Skip the GET listening stream initially — it exists for server-initiated requests (sampling/elicitation from remote servers); if the client doesn't advertise those capabilities, servers won't use them. |
| HTTP+SSE legacy | **Compatibility probe only** | ~15 lines: on POST failure with 400/404/405, GET the URL, expect `endpoint` event, then route POSTs there. No new features; it's Deprecated. |

**Lifecycle (2025-06-18 shape):** send `initialize` with latest supported version + capabilities `{roots:{listChanged:true}, sampling:{}, elicitation:{}}` — but advertise each client capability **only if the corresponding handler is compiled in and user-enabled**. Verify server's echoed version; disconnect on mismatch. Send `notifications/initialized`. Store `serverInfo.instructions` for the system prompt. Honor `tools.listChanged`/`resources.listChanged` notifications by re-listing.

**Statelessness hedge (cheap now, required later):** keep all outgoing calls routed through one `call(method, params)` choke point that already stamps a `_meta` map. When/if 2026-07-28 is adopted, this is where `io.modelcontextprotocol/protocolVersion` + `clientCapabilities` get injected and the handshake becomes a no-op. Don't build session state deeper than the client object.

**Supervision & reliability (stdio):**
- Spawn via CreateProcess with pipes (Windows) / posix_spawn (POSIX). Never through a shell — the command line comes from config and is a code-execution vector (see Security).
- Crash detection: EOF on stdout or process handle signaled → mark failed, reject all pending calls with a transport error, schedule restart with backoff (1s/2s/4s/8s/16s, cap, give up after N → surface to user). On restart: re-`initialize`, re-`tools/list`, re-subscribe resources. Do NOT copy Claude Code's stdio-excluded-from-reconnect behavior (#43177).
- Graceful shutdown: close stdin, wait ≤5s, terminate, wait ≤2s, kill. On Windows use Job Objects (kill-on-close) so orphaned children die with mcode.
- Timeouts: per-request default 30s for tools/list-type calls, 120s for tools/call (configurable); reset-on-progress-notification allowed but absolute max always enforced (spec-endorsed pattern). Timeout → send `notifications/cancelled`, stop waiting, ignore late response.
- `tools/list` pagination: follow `nextCursor` until absent.
- stderr: capture to a ring buffer per server; surface in a `/mcp`-style status view; never treat stderr as error (2025-11-25 clarified it's general logging).
- Elicitation handler: modal prompt rendering the flat schema; decline/cancel must be one keypress. Sampling handler: route through mcode's own model layer with user-visible approval; treat `modelPreferences.hints` as advisory substrings.

**Token budget management (mcode-native, no gateway dependency):**
1. Per-server tool toggle, default off for newly added servers; user enables deliberately.
2. On `tools/list`, compute schema token estimate client-side (chars/4 heuristic or tiktoken-lite) and display cost in the server status view.
3. Lazy tool loading: cache full schemas server-keyed; inject into model context only tools selected by a keyword/BM25 match against the current task + a always-loaded one-line index (`name — one-line purpose`). This replicates Anthropic's ~85% reduction pattern without an external gateway.
4. Warn when a single server's schemas exceed ~8K tokens (Notion-class bloat: 715 tok/tool × 24).
5. Honor 2026-07-28 `ttlMs` cache hints when servers send them (forward-compatible cheaply).

## Traps

- **Tool descriptions are executable influence.** Invariant Labs' tool-poisoning research (Apr 2025): malicious instructions inside tool `description`s induced Cursor's agent to read SSH keys and other servers' configs; "tool shadowing" lets one server's description steer how the model uses *another* server's tools ([invariantlabs.ai](https://invariantlabs.ai/blog/mcp-security-notification-tool-poisoning-attacks)). User confirmation UIs that show truncated tool info widen the human-model visibility gap.
- **Annotations are untrusted.** Spec: clients MUST consider tool annotations (`readOnlyHint` etc.) untrusted unless from trusted servers. Never auto-approve a call because `readOnlyHint: true`.
- **Rug pulls**: server approved once, then silently changes tool definitions. Pin/hash `tools/list` results; re-prompt on change.
- **Confused deputy / token passthrough**: never forward a token issued for server A to server B; MCP servers MUST NOT accept tokens not issued for them (spec). For mcode as OAuth client: implement RFC 8707 resource indicators; validate `state` (single-use, short expiry); validate `iss` per RFC 9207 when adopting 2026-07-28.
- **SSRF during OAuth discovery**: a malicious server's `WWW-Authenticate`/metadata URLs can point at `169.254.169.254` or internal IPs. Block private/link-local ranges, HTTPS-only except loopback, don't hand-roll IP parsers (octal/hex/IPv4-mapped tricks), pin DNS check→use ([security best practices](https://modelcontextprotocol.io/docs/2025-11-25/tutorials/security/security_best_practices.md)).
- **OAuth URL schemes**: MUST reject `javascript:`/`data:`/`file:`; MUST NOT open URLs via shell (`cmd.exe`/PowerShell) — documented RCE path.
- **stdout discipline**: any non-JSON-RPC byte on a stdio server's stdout corrupts the stream. Some real servers print banners. Tolerate-and-skip-until-valid-JSON is pragmatic; log it.
- **Batching**: don't implement; removed in 2025-06-18.
- **2026-07-28 is not incremental**: adopting it early breaks against the installed base (no handshake, sessions removed). Conversely, ignoring it forever strands you; the `_meta` choke-point hedge above is the middle path.
- **Windows stdio**: text-mode CRLF and UTF-16 default code pages break newline-delimited framing. Open pipes in binary mode, force UTF-8.

## Security checklist for mcode (client implementer)

**Scope: items 1–7 apply to the stdio transport (M7). Items 8–10 are OAuth/HTTP and are post-M7** — the HTTP transport ships second, so its checklist is written now and enforced then.

- [ ] stdio server launch: show full untruncated command, require explicit consent, warn on `sudo`/`rm -rf`/network/outside-workspace patterns; never launch via shell interpreter
- [ ] Spawned servers in restricted context: no inherited secrets env beyond declared needs; consider Job Object / sandbox for untrusted servers
- [ ] Tool descriptions + results passed to LLM wrapped in untrusted-data delimiters; never interpolated as system instructions
- [ ] Tool annotations untrusted; human confirmation required for any call not provably read-only in mcode's own policy (not the server's hint)
- [ ] Hash/pin `tools/list` results; re-approval on change (rug-pull defense)
- [ ] Cross-server isolation: config option to run a server with no visibility into other servers' tool results in prompt assembly (mitigates tool shadowing)
- [ ] OAuth: RFC 8707 resource indicators; audience validation; no token passthrough; `state` single-use + bound to consent; credentials keyed by issuer (2026-07-28 rule, adopt early)
- [ ] OAuth discovery URLs: HTTPS-only (loopback exception), private-IP blocklist incl. 169.254.0.0/16, no auto-redirect-following, no hand-rolled IP parsing
- [ ] Authorization URLs: `http(s)` schemes only; opened via OS API, never shell
- [ ] `MCP-Session-Id` stored securely, sent only to the issuing origin; DELETE session on exit
- [ ] Streamable HTTP: verify TLS; servers must 403 bad `Origin` (mcode as server-for-local-tools case, if any)
- [ ] Per-request timeouts + absolute max; cancellation notification on timeout; pending-call cleanup on transport death
- [ ] Restart backoff with cap; pending calls rejected (never replayed blind) on crash
- [ ] Audit log: server name, tool, args digest, duration, outcome — local file, user-inspectable

## Open questions

- Will the installed fleet actually migrate to 2026-07-28's stateless model, or will 2025-06-18 remain the de-facto interop target for years (like TLS 1.2)? Monitor registry `protocolVersion` distribution before committing.
- Does mcode ever want to *be* an MCP server (expose its own tools to other agents)? 2026-07-28's `server/discover` + stateless model makes that cheaper; defer until asked.
- Elicitation vs MRTR: implement elicitation now (works against 2025 fleet) and MRTR later, or abstract both behind one "server needs input" client surface? Leaning: one internal interface, two adapters.
- Tasks (experimental in 2025-11-25, extension in 2026-07-28): skip until a real long-running server needs it.

## Sources

Fetched directly (read):
- https://modelcontextprotocol.io/specification/2025-06-18/changelog.md
- https://modelcontextprotocol.io/specification/2025-03-26/changelog.md
- https://raw.githubusercontent.com/modelcontextprotocol/modelcontextprotocol/main/docs/specification/2025-11-25/changelog.mdx
- https://raw.githubusercontent.com/modelcontextprotocol/modelcontextprotocol/main/docs/specification/2026-07-28/changelog.mdx
- https://modelcontextprotocol.io/specification/2025-11-25/basic/transports.md (+ 2025-06-18 and 2024-11-05 versions)
- https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle.md
- https://modelcontextprotocol.io/specification/2025-06-18/client/elicitation.md
- https://modelcontextprotocol.io/specification/2025-06-18/client/roots.md
- https://modelcontextprotocol.io/specification/2025-06-18/client/sampling.md
- https://modelcontextprotocol.io/specification/2025-06-18/server/tools.md
- https://modelcontextprotocol.io/docs/2025-11-25/tutorials/security/security_best_practices.md

Fetched via search (result summaries, URLs retained):
- https://github.com/zhang-liz/mcp-token-benchmark (token measurements)
- https://www.anthropic.com/engineering/advanced-tool-use (vendor engineering blog; labeled vendor)
- https://www.anthropic.com/engineering/code-execution-with-mcp (vendor)
- https://invariantlabs.ai/blog/mcp-security-notification-tool-poisoning-attacks
- https://github.com/TBXark/mcp-proxy ; https://github.com/docker/mcp-gateway ; https://metamcp.org
- https://blog.modelcontextprotocol.io/posts/2025-12-09-mcp-joins-agentic-ai-foundation/ ; https://mcpindex.ai/stats ; https://devtoolhub.com/mcp-registry-by-the-numbers/
- https://github.com/anthropics/claude-code/issues/43177 ; https://github.com/anthropics/claude-code/issues/74329
- https://github.com/hkr04/cpp-mcp ; https://ibm.github.io/tsar-mcp/
