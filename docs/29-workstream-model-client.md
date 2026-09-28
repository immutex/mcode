# Parallel Workstream 1 — Model client (TLS + transport + retry)

> **TL;DR:** Make `mcode exec "..."` actually reach a model. Deliver a streaming
> client over the frozen `model::model_client` interface: TLS, request rendering
> from a descriptor, SSE→`chat_event` bridging, the retry policy, and credential
> resolution. **Depends on nothing another branch is writing.**

Branch: `feat/model-client`
Base: `master` **after Phase 0** (`docs/26` §Phase 0) — P3 and P4 land there
Owner: agent 1

---

## Why this slice

Three facts decide the split:

1. `http_client::send` returns `errc::unsupported` for every `https://` URL —
   **TLS is not linked**. No provider can be reached without fixing this first.
2. `mcode exec` prints `"no model client in M0"` and exits 4. Nothing streams.
3. `docs/15` §Provider abstraction already owns the retry policy, so the policy
   is a specification to implement, not a design to invent.

This slice is **the transport**: everything between "a canonical request" and "a
sequence of canonical events". It touches no loop state, no tool, and no context
assembly — those are the other two branches.

---

## Scope

### In scope

| Item | Deliverable |
|---|---|
| TLS | OpenSSL linked; `https://` works; `http://` unchanged |
| Request rendering | `model::render_request` — byte-stable body from a descriptor |
| Credentials | `auth_header_value`, `resolve_api_key` |
| Stream client | `model::http_model_client : model_client` |
| SSE bridge | `sse_parser` → `delta_applier` → `chat_event` sink |
| Retry | Full-jitter backoff, cap 30 s, max 5 attempts, `Retry-After` |
| Error taxonomy | `transient` / `fatal` / `context_overflow` classification |
| Usage | Fold `chat_event::kind::usage` into a `model::usage` |
| CLI wiring | `exec` sends one real request and streams text to stdout |
| Provider lookup | Resolve a descriptor by name from the registry the Lua provider extension already populates |
| Capabilities table | The compiled-in JSON table `docs/15` specifies, keyed by model id, fail-closed on an unknown id |
| `net/http_client` | **Consume** Phase 0's `{status, headers, body}` failure outcome (P4) — do not add it yourself |
| Tests | Unit + a loopback integration test with a stub HTTP server |

### Explicitly NOT in scope

- **The loop state machine** — workstream 2. This branch emits events to a sink
  and stops. It has no notion of `Act`/`Observe`/`Verify`.
- **Any tool** — workstream 3.
- **Context assembly / compaction** — workstream 2 owns the prompt it sends.
  This branch renders *whatever request it is handed*.
- **A real provider key or a live API call in CI.** Every test uses a loopback
  stub. A test that needs a secret is a test that never runs.
- **Provider descriptors.** `extensions/providers/init.luau` already defines the
  three reference providers as data. You consume them; you do not re-declare
  them (`docs/26` D3).

---

## The interface you implement (already on `master`)

`src/mcode/model/client.hxx` — do not change its signatures. If a signature is
genuinely wrong, say so in the PR description rather than editing it; two other
branches compile against it.

```cpp
struct stream_request { chat_request request; provider_descriptor provider;
                        std::string api_key; capabilities caps; };
using event_sink = std::function< void( const chat_event& ) >;

class model_client {
public:
    virtual auto stream( const stream_request&, const event_sink& ) -> status = 0;
};

[[nodiscard]] auto render_request( const stream_request& ) -> result< std::string >;
[[nodiscard]] auto auth_header_value( const auth_spec&, std::string_view api_key ) -> result< std::string >;
[[nodiscard]] auto resolve_api_key( const auth_spec&,
    const std::function< std::optional< std::string >( std::string_view ) >& from_config ) -> result< std::string >;
```

---

## Tasks

### T1 — Link TLS

`conanfile.py`: add `openssl/3.x`. `docs/14` leaves OpenSSL-vs-libcurl open; take
OpenSSL, because Beast is already the HTTP stack and libcurl would add a second.

| Check | Requirement |
|---|---|
| `https://` | Succeeds against a real host |
| `http://` | Still succeeds (a local Ollama endpoint uses it) |
| Binary size | Measure and report. `docs/01` caps at 25 MB, target 5–9 MB. **If it exceeds 25 MB, stop and report** — that is a decision, not a bug |
| Certificate verification | **On.** A verify-disabled build is a security bug, not a convenience |
| Static linkage | `docs/01`: **no third-party runtime dependencies on any platform.** Set `openssl/*:shared=False` in `default_options` alongside the existing `boost/*:shared=False`-style entries. A dynamically linked `libcrypto` silently breaks the "fully static on Windows and Linux" budget, and it fails at *runtime* on a clean machine, not at link time |
| CI cache | No action needed: `ci.yml` runs `conan install --build=missing` and keys the cache on `hashFiles('conanfile.py', …)`, so adding the dependency invalidates it correctly |

Use `boost::asio::ssl::context` with `tls_client`. Set SNI from the URL host.
Reuse the existing `parse_url`.

**Traps**
- Beast's `ssl_stream` needs the handshake awaited before the write.
- `SSL_set_tlsext_host_name` for SNI — omitting it fails on shared hosts.
- The existing `tcp_stream` code path must stay for `http://`; do not force
  everything through TLS.

### T2 — `render_request`

Byte-stable rendering. This is the prompt-cache contract from `docs/05`:
**fixed key order, sorted tool schemas, no timestamps, no cwd.**

```
{ "model": …, "messages": […], "tools": […], "max_tokens": …, "temperature": …,
  "stream": true, "response_format": … }
```

- Field NAMES come from `provider_descriptor::request` (already implemented;
  defaults are `model`/`messages`/`tools`/`max_tokens`/`temperature`/
  `response_format`).
- Roles are rendered through `request.role_system` etc.
- **Tool schemas sorted by name**, once. An unsorted array is a different byte
  string per run and invalidates the cache silently.
- Omit a field entirely when it is empty/zero, rather than sending `null`.
  `temperature < 0` means "unset" (`chat_request::temperature` defaults to
  `-1.0`).
- **Breakpoints: render them when `cache.mode == explicit_markers`, ignore them
  otherwise.** `cache_mode::implicit` means the provider does it itself and
  sending markers is noise; `cache_mode::none` means it is unsupported. Only
  `explicit_markers` requires the markers — Anthropic's
  `cache_control: {type: "ephemeral"}`, at most 4, and `docs/15` warns that
  breakpoints on per-turn content write every time and never hit.
  `chat_request::cache.breakpoints` holds byte offsets into the rendered prompt.
  Workstream 2 computes them; you render them.

  **This is the seam where the two branches meet, so state the contract
  precisely:** the offsets are into the *rendered* string, and a marker occupies
  bytes, which shifts every later offset. Apply them **right-to-left** so each
  offset is still valid when it is used. Assert this with a test that has two
  breakpoints — a left-to-right implementation corrupts the second one, and the
  symptom is a silent cache miss, not an error.

**`mcode::json::document` cannot build this body, and Phase 0 (P3) fixes that.**
Verified: its mutable API is `set_string` and `set_int` only, both flat — no
arrays, no nested objects. A request body is `messages[]` of nested objects
containing `tools[]`. Phase 0 extends `json::document` with `set_array` /
`set_object` (`docs/26` §Phase 0). You consume it; do not add a second JSON
writer, and do not hand-roll the body as string concatenation.

**Key order: sorted, and it is a tested contract.** `dump()` passes
`YYJSON_WRITE_PRETTY` or `YYJSON_WRITE_NOFLAG` — there is no
`YYJSON_WRITE_SORT_KEYS` in this version, and the *parse* path re-emits the source
order verbatim. The *mutable* writer emits sorted key order because it holds
members in a `std::map`, and `test_json.cxx` asserts that two documents built in
different insertion orders serialize identically. Phase 0 extended that rule
recursively, so nesting inherits it.

So the rule is: build with the setters and you get sorted, order-independent
bytes. Do not switch the container to a vector to "control" the order — that
breaks the determinism the cache depends on.

**Acceptance:** two calls with equal inputs produce byte-identical output.
Assert it. Use `tools` in a non-sorted insertion order in the test so the sort
is actually exercised.

### T3 — Credentials

```
auth_spec { source from; string name; string header; string scheme = "Bearer"; }
```

- `source::none` → no header, empty value. Only valid for a local endpoint;
  `validate()` already enforces that.
- `source::environment` → `std::getenv(name)`. **Missing → error.** Do not send
  the request with an empty key; a 401 with no explanation is the worst outcome.
- `source::config` → the `from_config` callback. Missing → error.
- Header value is `scheme + " " + key` when `scheme` is non-empty, else the bare
  key (Anthropic's descriptor uses `scheme = ""`).

**Acceptance:** a test per source, including both missing-value errors.

### T4 — `http_model_client`

```cpp
class http_model_client final : public model_client {
public:
    explicit http_model_client( http_client& transport );
    auto stream( const stream_request&, const event_sink& ) -> status override;
};
```

Flow:
1. `render_request`.
2. Build `http_request`: URL = `provider.endpoint`, headers = `Content-Type:
   application/json`, `Accept: text/event-stream`, the auth header, plus
   `provider.extra_headers_json` parsed and merged.
3. `delta_applier applier{ provider }` — already implemented, already tested.
4. `transport.stream_sse( req, [&]( sse_event&& e ) { … } )`:
   - feed `e.event` and `e.data` to `applier.feed( name, data )`
   - forward each produced `chat_event` to `sink`
5. On stream end, call `applier.finish()` and forward those events too.
6. `applier.finish()` **always** appends a `turn_done`, terminal or not
   (verified in `delta_applier.cxx`). So `turn_done` is not the truncation signal.
   Use `applier.saw_terminal_event()`: when it is false, the turn was truncated —
   surface the partial text and fail. Do not add a second `turn_done`; the applier
   already emitted one and a duplicate would read as a completed turn.

**Blocker: `stream_sse` cannot report what T5 needs to classify.**

Verified in `net/http_client.cxx`: a non-200 response is collapsed to
`fail( errc::protocol, "SSE request returned HTTP " + status )`. The typed status,
the response **headers**, and the error **body** are all discarded. T5 requires
all three:

| T5 needs | Why | Available today |
|---|---|---|
| the numeric status | 429 vs 5xx vs 401 | string only |
| the error **body** | `rate_limit_exceeded` vs `insufficient_quota` — the entire 429 split | **no** |
| `Retry-After` | the backoff delay | **no** |

`Retry-After` is not a header you can re-derive; the 429 split is not decidable
from the status. **Phase 0 (P4) adds that outcome** — `docs/26` §Phase 0. You
consume it. If Phase 0 has not landed, stop and say so rather than editing
`net/http_client` yourself: it is a shared file and a second version of the failure
path is worse than the missing field.

Do not work around this by string-matching `"HTTP 429"`. The body is the only
place the quota distinction lives.

**Traps**
- `applier.feed` returns `result<vector<chat_event>>` — a malformed payload is an
  error to propagate, not to skip.
- `sse_parser::event_callback` takes `sse_event&&`.
- **`feed` must not be called with an empty data payload** — the parser already
  handles `[DONE]`; check what it does before adding a second guard.
- The `[DONE]` sentinel and declared `terminal_events` both set `terminal_seen_`
  in the applier. Read `delta_applier.cxx` before adding a third path.

### T5 — Retry and error taxonomy

Owned by `docs/15`: **exponential backoff with full jitter, cap ~30 s, max 5
attempts, honour `Retry-After`.**

Classify each failure into the **canonical taxonomy from `docs/15:50`** — those
four names are the vocabulary, and `errc` maps onto them:

| Class | Conditions (`docs/15:50`) | Action |
|---|---|---|
| `Transient` | 429 **non-quota**, 5xx, 529, timeouts, network | retry |
| `Fatal` | 401/403/402/**413**, quota codes, invalid schema | no retry, surface |
| `ContextOverflow` | the body names context length / too many tokens | trim or compact, then **optionally retry once** |
| `ContentFilter` | a refusal | no retry, surface as its own outcome |
| **Anything after the first event reached the sink** | — | **fatal, never retry** |

Two traps in that table:

- **429 is two different things.** `rate_limit_exceeded` retries;
  `insufficient_quota` / `*_spend_limit_exceeded` / `credit_balance_exhausted`
  **never** retry — they are billing, and retrying burns the user's time on a
  request that cannot succeed. `docs/15:46` owns that distinction.
- **413 is `Fatal`, not `ContextOverflow`.** The doc is explicit. Do not
  "helpfully" treat it as overflow.

The last row is the important one: a partially-consumed stream must not be
replayed, because the sink has already seen tokens and the caller has already
appended them to history. Surface the partial text plus the error; the loop
re-issues the turn.

`docs/15:99` also notes official SDKs auto-retry twice by default — **do not add
your own layer on top of theirs**. This client is the retry layer; there is no
other.

**Acceptance:** a test per class, including the 429 split (a rate-limit code
retries, a quota code does not). The "no retry after first event" case is the one
to write first — it is the one whose absence corrupts history.

Full jitter: `sleep = random(0, min(cap, base * 2^attempt))`. Seed the RNG
injectably so the test is deterministic.

### T6 — Usage accounting

Fold `chat_event::kind::usage` into a `model::usage` via the existing
`usage::add` (it takes the max, which is correct for both delta-reporting and
cumulative providers). Expose `usage accumulated_usage() const` on the client so
the loop can charge the budget.

Cost: `compute_cost( caps, usage )` already exists. **Never estimate tokens from
a tokenizer** — provider-reported only.

### T7 — CLI wiring

`cli_commands.cxx`'s `run_exec` currently publishes two events and exits 4.

**The provider descriptors already exist** — `extensions/providers/init.luau`
registers `openai-chat-completions`, `openai-responses` and `anthropic-messages`
as data, and `tests/test_dogfood_providers.cxx` covers the registration. **Do not
write a second set of descriptors in C++.** The extension is the dogfood test for
the descriptor (`docs/26` D3), and duplicating it here defeats the point of that
test.

Change `run_exec` to:
1. Load config through the existing `support::config` layers. Read
   `model.provider`, `model.model`, `model.api_key_env`, and `model.tier.act`
   (`docs/22` §`[model]`).
2. Resolve the descriptor by name from the `provider_registry` that the extension
   loader populated. **A provider name that matches no descriptor is a fatal,
   named error** — not a silent fallback to a default.
3. `model.base_url` overrides the descriptor's `endpoint`, for a local
   [OI]-compatible endpoint. An `http://` base URL must work; that is the
   offline path.
4. `resolve_api_key` against `model.api_key_env`.
5. Build one `chat_request` with a single user message. `effort` = `medium`
   (`docs/15` §Reasoning-effort scheduling: `act` is `medium`).
6. Stream to stdout, writing text deltas as they arrive.
7. On success emit `run.end` with `exit_code::success`; on a provider failure,
   `exit_code::provider_error` and the message on stderr.

**This is deliberately a stub of the loop** — one request, no tools. Workstream 2
replaces the body. Keep the change small so the merge is small.

**Trap:** the provider registry is populated by the extension loader, so
`run_exec` depends on extension loading having happened first. If the loader is
not wired into this path yet, that is a finding to report — do **not** work around
it by hardcoding a descriptor.

**Acceptance:** `mcode exec "hi"` against the loopback stub prints streamed text
and exits 0. Record the transcript in the PR.

### T8 — Capabilities, and the fail-closed rule for an unknown model

`stream_request.caps` is a required input, and **nothing in the repository
produces it**. Verified: `[model]` in `docs/22` has no pricing or capability keys,
the provider descriptor has none, and there is no model registry. Without a
source, `capabilities{}` is zero-initialised, `compute_cost()` returns `0.0`,
`session_budget::usd_used` never advances — and **budget enforcement silently
becomes a no-op**. A check that can never fail is worse than no check
(`AGENTS.md` §Correctness).

`docs/15` already specifies the fix: *"`Capabilities` per model id, data-driven
from a compiled-in registry (JSON table in binary), not code."* Build it:

- One JSON table embedded in the binary, keyed by model id, each entry carrying
  `caching`, the `supports_*` flags, `context_window`, `max_output_tokens`, and
  the four prices.
- `lookup( model_id ) -> std::optional< capabilities >`.
- **Unknown model id → fail closed.** Do not default to zeros and proceed. Two
  acceptable behaviours, pick one and state it: refuse the run with a named error
  naming the unknown id, or run with USD tracking explicitly marked unavailable
  and the budget charged on steps and tokens only. What is **not** acceptable is
  reporting `$0.00` spent for a model whose price is unknown.
- `docs/15` owns the table's contents. Do not invent prices — an invented price is
  a fabricated benchmark, which `AGENTS.md` forbids. Seed it with entries you can
  source, and leave the rest absent so the fail-closed path fires honestly.

**Acceptance:** a test asserting an unknown model id does not yield a zero-cost
`capabilities`, and that a known id yields the table's values.

### T9 — The request shape is not expressible for every provider

`request_spec` carries field **names** (`model`, `messages`, `tools`,
`max_tokens`, `temperature`, `response_schema`) and four role names. It carries no
field **shapes**. That is enough for an [OI]-compatible endpoint and **not enough
for Anthropic**, whose body is `{model, max_tokens, system, messages:[{role,
content:[blocks]}]}` — a top-level `system` string instead of a system message,
and content as a block array instead of a string.

The stream direction is fully descriptor-driven (`delta_applier`, D2's acceptance
criterion). The request direction is not, and `extensions/providers/init.luau`
ships `anthropic-messages` with a `stream` block only.

**Scope decision for v1: render the [OI] Chat Completions request shape, and
refuse a descriptor you cannot render.** Concretely:

- `openai-chat-completions` works.
- `openai-responses` and `anthropic-messages` are resolved but rejected with a
  named "request shape not supported" error until `request_spec` grows shape
  fields.

State this in the PR and in `docs/15` — a provider that loads, validates, and then
fails at first request is exactly the silent failure the descriptor rules exist to
prevent. The alternative (extending `request_spec` with shape fields now) is a
design change that belongs in a doc first, not in a branch.

**Acceptance:** a test asserting `anthropic-messages` produces a named, actionable
refusal rather than a malformed request.

---

## Tests

| Test | Asserts |
|---|---|
| `render_request is byte-stable` | Two calls, equal bytes; tools given out of order |
| `render_request omits unset fields` | No `temperature` when negative |
| `breakpoints apply right-to-left` | Two breakpoints, both offsets still valid; implicit mode renders none |
| `credentials resolve per source` | env, config, none, and both missing-value errors |
| `a transient failure is retried` | Stub returns 503 then 200; sink sees the 200 |
| `a fatal failure is not retried` | Stub returns 401; exactly one attempt |
| `a partial stream is never retried` | Stub sends 2 events then drops; one attempt, partial text surfaced |
| `retry honours Retry-After` | Header value drives the delay |
| `usage folds by max` | Two usage events, cumulative; result is the max |
| `https is not refused` | `parse_url` + client path accepts an `https` URL |
| `the loopback stub drives a full turn` | End-to-end: SSE text → sink → stdout |
| `a failed SSE response carries its status and body` | T4's blocker: 429 body reaches the classifier |
| `an unknown model id does not price as free` | T8's fail-closed rule |
| `an unrenderable request shape is refused by name` | T9 |

Loopback stub: a minimal TCP server on `127.0.0.1:0` in the test process,
speaking canned HTTP/SSE. **No external network in any test.**

---

## Acceptance criteria (PR is not ready until all hold)

1. `mcode exec "hi"` against the loopback stub streams text and exits 0.
2. `mcode exec "hi"` against a real provider works manually (key from env,
   **never committed**, transcript in the PR description with the key redacted).
3. Binary size measured and reported; ≤ 25 MB.
4. `ctest` 100% pass; `python _clgate.py` exit 0; smoke exit 0; bench gate pass.
5. Every new file ≤ 600 lines; no `docs/NN` citations in code; no magic numbers;
   trailing return types; spaced parens; `≤4` positional parameters.
   **Comments: default none.** Justified only where a reader would otherwise get
   the code *wrong*; lowercase; **max one line**; never restate a name, narrate the
   next line, or explain the obvious. A multi-paragraph comment on a one-line field
   is a review rejection, not a style preference — the rationale belongs in the
   commit message or the doc that owns the decision.
6. `docs/14` updated: OpenSSL is now linked, with the measured size delta. If
   `docs/14`'s open question is resolved by this, resolve it there.
7. `docs/15` unchanged unless the implementation contradicted it — then fix
   whichever is wrong and say which in the PR.

---

## Merge-conflict surface

Files this branch touches that another branch also touches:

| File | Other branch | Resolution |
|---|---|---|
| `conanfile.py` | none | safe |
| `src/CMakeLists.txt` | 2 and 3 | **Expected conflict.** Add your sources in the same block, alphabetical. Trivial to resolve |
| `src/cli_commands.cxx` | 2 (replaces `run_exec`'s body) | **Expected conflict.** Yours adds the single-request path; branch 2 replaces it. Prefer branch 2's structure and keep your provider selection |
Do **not** touch `agent/loop.*`, `model/types.*`, `model/provider.*`,
`model/delta_applier.*`, `core/registry.*`, or `tools/` — those are frozen or
owned elsewhere.

---

## Risks

| Risk | Mitigation |
|---|---|
| Binary exceeds 25 MB with OpenSSL | Measure in T1 before building on it. If it breaches, report rather than proceeding — the fallback is a different TLS choice, which is a decision |
| Certificate verification disabled "to make it work" | Explicitly forbidden. A test asserts verification is on |
| Retry replaying a partial stream | T5's first test |
| Byte-unstable rendering | Asserted, with a deliberately unsorted tool list |
| Provider drift from the descriptor | The descriptor is data; if a real provider needs something it cannot express, that is a finding for `docs/15`, not a hardcoded branch here |

## Sources

- `docs/15-model-layer.md` — the canonical error taxonomy, the retry policy this
  branch implements, provider wire shapes, cache pricing
- `docs/14-cpp23-stack.md` — the TLS choice and the size budget it has to fit
- `docs/05-context-engineering.md` — the cache-layout rule that makes
  `render_request` byte-stability a contract rather than a nicety
- `docs/22-config-and-cli.md` — `[model]` config keys and the exit codes
- `docs/01-north-star.md` — binary-size and static-linkage budgets
- `docs/26-first-batch.md` §Decision 2 — the provider seam and the dogfood rule
- `extensions/providers/init.luau` — the three reference descriptors, verified
  this session
- `docs/26-first-batch.md` §Phase 0 — P3 (`json::document`) and P4 (`net/http_client`), the two interfaces this branch consumes but does not add
