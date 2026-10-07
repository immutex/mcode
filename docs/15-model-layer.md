# Model Layer: Providers, Caching, Inference Economics

> TL;DR: Normalize every provider behind one streaming ChatRequest/ChatEvent interface, lay out prompts append-only for prefix caching (80-90% input savings on hits), and route by task tier — frontier for planning, small models for mechanical side tasks.

## State of the field

### Wire APIs

| Provider | Native API | Streaming | Notes |
|---|---|---|---|
| OpenAI | Chat Completions (`/v1/chat/completions`) + Responses (`/v1/responses`) | SSE | Responses is the new primary surface: `input` items, `previous_response_id`, `instructions`, `store`, background/cancel, built-in tools (web_search, file_search, MCP, apply_patch, code_interpreter). CC is stateless `messages`. |
| Anthropic | Messages (`/v1/messages`) | SSE | Typed SSE events; `count_tokens` endpoint; thinking blocks. |
| Google Gemini | `generateContent` + OpenAI-compat shim (`/v1beta/openai/`) | SSE | Compat shim covers Chat Completions incl. function calling; leaks differences under load. |
| AWS Bedrock | Converse API | SSE | `toolConfig.tools[].toolSpec`, `inputSchema.json`, `stopReason: "tool_use"`, `toolUseId` round-trip. |
| Local | Ollama / llama.cpp / vLLM all expose OpenAI-compat `/v1/chat/completions` [UNVERIFIED — vendor docs not fetched] | SSE | Tool-call quality varies by model + parser. |

Anthropic documents that its SSE stream may emit `ping` events anywhere and unknown future event types must be tolerated. OpenAI Responses streaming events carry a monotonically increasing `sequence_number` — use it to detect drops/reordering. Mid-stream errors arrive as `error` events after HTTP 200 (both providers) — the normal error taxonomy does not apply once streaming started.

### Streaming: partial tool-call deltas

- **OpenAI CC**: tool calls arrive as `choices[0].delta.tool_calls[]` fragments keyed by `tool_call.index`; `function.arguments` is a partial JSON *string fragment* (split arbitrarily, e.g. `{"ci` / `ty\":\"Bos` / `ton\"}`). Accumulate string fragments per index; parse only after `finish_reason: "tool_calls"` or `[DONE]`. The first fragment usually carries `id`+`name`; later ones only `arguments`. Multiple parallel calls = multiple indices.
- **OpenAI Responses**: semantic events instead: `response.created` → `response.in_progress` → `response.output_item.added` → `response.output_text.delta` / `response.function_call_arguments.delta` → `response.output_item.done` → `response.completed` (or `response.failed`/`response.incomplete`). `response.output_item.done` carries the finalized item — OpenAI explicitly warns the `added` variant of a reasoning item's `encrypted_content` may be incomplete; only the `done` payload is safe to persist.
- **Anthropic**: `message_start` → (`content_block_start` → `content_block_delta`* → `content_block_stop`)* → `message_delta` (final `stop_reason`, cumulative usage) → `message_stop`. Delta types: `text_delta`, `input_json_delta` (tool input, `partial_json` fragments keyed by block `index`), `thinking_delta`. Parse accumulated tool JSON only at `content_block_stop`.

Robust parser rules (all three): key state by (choice/index) → append fragments → never parse mid-stream → tolerate unknown event types → treat transport close before terminal event as failure, not completion.

### Tool-calling formats

| Concern | OpenAI | Anthropic | Gemini | Bedrock |
|---|---|---|---|---|
| Decl | `tools[].function{name,description,parameters}` | `tools[]{name,description,input_schema}` | `tools[].functionDeclarations[]{name,description,parameters}` | `toolConfig.tools[].toolSpec{name,description,inputSchema.json}` |
| Schema types | JSON Schema | JSON Schema | Uppercase `OBJECT`/`STRING`... | lowercase JSON Schema |
| Call out | `tool_calls[]{id,function{name,arguments-string}}` | `content[]{type:"tool_use",id,name,input-object}` | `functionCall{name,args}` | `content[].toolUse{name,input,toolUseId}` |
| Result back | `role:"tool", tool_call_id` | `content[]{type:"tool_result",tool_use_id}` | `functionResponse{name,response}` | user msg `toolResult{toolUseId,content,status}` |
| Stop | `finish_reason:"tool_calls"` | `stop_reason:"tool_use"` | (functionCall present) | `stopReason:"tool_use"` |

Normalization: canonical internal form = JSON-Schema function decl + `{call_id, name, args: string-of-JSON}` + typed result message. Per-provider codec converts. Note Gemini's `args` is an object while OpenAI's `arguments` is a string; Bedrock requires echoing `toolUseId` exactly. Anthropic also supports native strict structured output via `output_config.format` (JSON outputs; GA) and strict tool schemas.

### Reasoning controls

- OpenAI: `reasoning.effort` (Responses) / `reasoning_effort` (CC). Levels `minimal|low|medium|high`; `none` only on GPT-5.1+; `xhigh` on some newer models. Default `medium` pre-5.1. Reasoning tokens reported in `usage.output_tokens_details.reasoning_tokens`; billed as output.
- Anthropic: pre-4.6 `thinking:{type:"enabled",budget_tokens:N}`; 4.6 deprecated in favor of `thinking:{type:"adaptive"}` + `output_config.effort`; **Claude 4.7+ rejects `budget_tokens` with a 400**. Interleaved thinking = reasoning between tool calls, recommended for agent loops.

### Error taxonomy & retries

OpenAI: 429 `rate_limit_exceeded` (retry; honor `Retry-After`) vs 429 `insufficient_quota` / `*_spend_limit_exceeded` / `credit_balance_exhausted` (never retry — billing). 529 overloaded + 5xx: bounded backoff + jitter. Rate-limit headers: `x-ratelimit-remaining-requests/-tokens`, `-reset-*`. Official SDKs auto-retry twice by default — don't double-retry above your own layer.

Anthropic: `invalid_request_error`(400) `authentication_error`(401) `billing_error`(402) `permission_error`(403) `not_found_error`(404) `conflict_error`(409) `request_too_large`(413) `rate_limit_error`(429) `api_error`(500) `timeout_error`(504) `overloaded_error`(529). A tier spend-cap 429 has **no `retry-after`** and won't clear on retry — detect and surface. SDKs retry transient errors twice, honoring `retry-after`.

Canonical mcode taxonomy: `Transient` (429 non-quota, 5xx, 529, timeouts, network), `Fatal` (401/403/402/413, quota codes, invalid schema), `ContextOverflow` (trim/compact then optionally retry once), `ContentFilter`. Respect `Retry-After`; else exponential backoff + full jitter, cap ~30s, max ~5 attempts.

## What works / what doesn't

**Cache economics (the big lever).**

| Provider | Write cost | Read (hit) cost | TTL | Mechanism |
|---|---|---|---|---|
| Anthropic | 1.25× input (5m), 2× (1h) | **0.10× input (90% off)** | 5 min default, sliding; 1h opt-in | `cache_control:{type:"ephemeral"}` breakpoints (max 4); auto-advance mode available |
| OpenAI (gpt-5 family) | free (implicit) | **0.10× input** (e.g. gpt-5 $1.25 → $0.125 /MTok) | ~5-10 min sliding, 1024-token min prefix | Automatic prefix caching; `prompt_cache_key` to bucket; usage: `prompt_tokens_details.cached_tokens` |
| OpenAI (gpt-6 family, per current pricing page) | 1.25× input (e.g. sol $2.50 vs $2.00) | 0.10× input ($0.20) | same | Explicit `prompt_cache_breakpoint:{mode:"explicit"}` on input items + `prompt_cache_options.ttl`; usage splits `cache_write_tokens`/`cached_tokens`. Long-context tier = 2× input. |
| Gemini explicit | storage $1.00/MTok/hr (flash-lite tier) + cached-use fee (~$0.01-0.03/MTok) | cached-use fee | default 1h, any `ttl:"300s"`-style, updatable | Named `cachedContents` objects, `v1beta`, tied to model; implicit caching also exists, no guarantee |

Net: **cache-friendly layout saves 80-90% of input-token cost on hits across all three majors** — with 5-minute sliding TTLs, that means append-only conversation history where each turn re-reads the entire prefix at 10% price and writes only the delta at 1.25×. A 100k-token prefix on turn N costs 90% less than re-sending. Cache misses (layout churn) flip this into a 25% penalty on OpenAI/Anthropic — layout discipline is not optional.

**What works**: prefix-stable system prompt + tool schemas; append-only history; tool results as trailing messages. **What doesn't**: timestamps/`cwd`/random IDs inside the prefix; JSON-serializing messages with unstable key order; parallel fan-out requests before the first response lands (Anthropic: cache available only after first response begins); Anthropic >4 breakpoints or breakpoints on per-turn content (writes every time, no hits).

**Small-model side tasks**: Haiku 4.5 ≈ 73.3% vs Sonnet 4.5 77.2% SWE-bench Verified at $1/$5 vs $3/$15 per MTok, ~800ms vs ~1.2s latency (vendor-adjacent comparisons; treat as directional). For title generation, summarization, and search-query synthesis, small models are fine; for constraint-following under load, third-party testing repeatedly reports Haiku-class models dropping negative constraints and multi-part instructions (community testing, not benchmark-grade [UNVERIFIED]). BFCL overall (Sep 2026 mirror): Opus 4.5 77.5%, Sonnet 4.5 73.2%, GLM-4.6-thinking 72.4% (best open), Haiku 4.5 68.7%, o3 63.0% — tool-calling is not a solved commodity even at the frontier.

**Local models**: usable for side tasks and offline dev with OpenAI-compat endpoints; multi-turn agentic tool use remains clearly below frontier (BFCL leaderboard top group is all hosted models). llama.cpp has native function-calling parsers; quality depends on the model's chat template, not just the runtime. Treat local as: dev-loop free tier + privacy tier, not default agentic brain.

**Token counting**: tiktoken is exact-ish for OpenAI text but diverges on tool-call messages and newer models' tokenizers (GitHub issue tiktoken#474; community reports). Anthropic has no public tokenizer — use `/v1/messages/count_tokens` (rate-limited, latency-bearing). Budget pre-flight with heuristic (chars/4 ± 20% guardband); settle accounts from `usage` fields, never estimates.

## Recommended design for mcode

### Provider abstraction

One canonical request/event model; per-provider codec + SSE parser. Providers are thin: translate + parse, own no policy.

```cpp
struct ToolSpec { std::string name, description; Json schema; };
struct Message  { Role role; std::vector<Block> blocks; }; // text | tool_call | tool_result | thinking
struct ChatRequest { std::string model; std::vector<Message> msgs;
                     std::vector<ToolSpec> tools; ToolChoice choice;
                     Effort effort; Json response_schema; CachePlan cache; };
struct ChatEvent {  // normalized; parsers fill, agent loop consumes
    enum Kind { TextDelta, ThinkingDelta, ToolCallDelta, TurnDone, Usage, Error } kind;
    // TextDelta/ThinkingDelta: payload; ToolCallDelta: index+id+name+args-fragment
};
struct Usage { int64 in, out, cached_read, cache_write, reasoning; double cost_usd; };
struct IProvider {
    virtual ~IProvider() = default;
    virtual async::Task<void> stream(const ChatRequest&, std::function<void(ChatEvent)>) = 0;
    virtual const Capabilities& caps() const = 0;  // caching mode, schema modes, effort levels, context window
};
```

- `Capabilities` per model id, data-driven from a compiled-in registry (JSON table in binary), not code. Codec failure on unsupported combos = `Fatal` error early.
- **The compiled-in table is a default, and the user's config extends it.** A model absent from both is refused at resolution with a named error rather than priced at zero — zero is the success encoding in `cost_usd`, so an unpriced model would report `$0.00` spent and turn budget enforcement into a silent no-op. That refusal assumes the table can enumerate every model, and it cannot: a gateway serving hundreds of ids is normal, and a binary that knows six of them is unusable against one. So `[models."<id>"]` lets a user price a model the table does not carry. The fail-closed rule is unchanged for a model neither source names; only an explicit user entry prices one. An entry that names no price at all is also refused, because it would reintroduce the zero. A model named in both places takes the config's values, so a stale compiled-in price is correctable without a rebuild.
  - An entry that sets `price_input` but not `price_cached_read` gets the input price for cached reads rather than zero. Zero is not "unknown", it is the claim that cached reads are free, and a gateway may report the *whole prompt* as a cache read even on a first request — InferHub does. Billing that at zero under-estimates the run by the entire input cost, so the budget stops late. Assuming no discount is the conservative default and the user can lower it explicitly.
  - An entry that sets `price_input` but not `price_cached_read` gets the input price for cached reads, not zero. Zero is not "unknown" — it is the claim that cached reads are free, and a gateway may report the *whole prompt* as a cache read even on a first request. InferHub does: with `price_cached_read` unset, `billed_input` is zero and a run costs `output × price_output` alone, under-estimating by the entire input cost. A budget that under-estimates stops late, so the default assumes no discount and the user can lower it explicitly.
  - The section is `[models."<id>"]` with the id quoted. A quoted key segment is taken verbatim, so the entry is keyed by the id exactly as written — `[models."cb/gpt-5.6-sol"]` — and no mangling is applied. Quoting is required because an id carries `/`, which a bare TOML key cannot hold. The resolution error prints the exact section to write.
- **Request direction is shape-limited in v1.** `request_spec` carries field names but no field shapes, so only the [OI] Chat Completions body is renderable. A descriptor whose wire shape is not chat-completions (`openai-responses`, `anthropic-messages`) is resolved and validated but **refused at first request with a named error** — a provider that loads, validates, and then fails at first request with a malformed body is exactly the silent failure the descriptor rules exist to prevent. Extending `request_spec` with shape fields is a design change that belongs in a doc first. The stream direction is fully descriptor-driven and unaffected.
- Cost accounting: registry carries $/MTok {input, cached_read, cache_write, output}; `Usage.cost_usd` computed per request from provider-reported usage only. Log per-turn + cumulative; display cumulative in status line.
- Retries live in the transport layer, keyed off the canonical taxonomy; `Retry-After` honored; never retry `Fatal`; never retry a partially-consumed stream — resurface partial text + error to the agent loop, which re-issues the turn (idempotent because tool results are recorded client-side). **This section owns the retry policy**; `03` and `04` reference it. Default: exponential backoff with full jitter, cap ~30 s, **max 5 attempts** per request.
- Fallback policy: on `Transient`-exhausted or provider outage → optional fallback chain, only when the user enabled it. **Fallback is run-boundary only, never mid-run.** Switching providers mid-run would strand the run on a capability mismatch (the fallback model may reject the tool-schema subset or lack the effort level), and it discards the cached prefix at the old provider's write rates. So: fail the run with partial state preserved, offer the fallback model for the next run. On `ContextOverflow` → compaction, not a model switch. Abort: Ctrl-C cancels the HTTP body; record the partial turn; tool calls never execute after abort.

### Request-side capabilities: declared, attempted, downgraded

A gateway's wire format says what it *could* express; only the gateway says what it *implements*. Two gateways can accept the same body shape and differ on whether `strict`, `parallel_tool_calls` or a prefilled assistant turn does anything. Each is therefore a declared flag on the descriptor, defaulting to the conservative answer, and a technique is attempted only when its flag is set:

| Feature | Default | Effect when set |
|---|---|---|
| `strict_tools` | off | `strict: true` per function plus the strict schema shape (`06`) |
| `parallel_tool_calls` | off | `parallel_tool_calls: false` — the loop serializes calls anyway, so naming it buys nothing unless the gateway honours it |
| `prefill_text` | empty | An assistant turn with this content, sent last, so the answer begins mid-turn and skips a preamble |
| `response_format_with_tools` | off | Keep the JSON response format when a tool list is present (`06`) |

Two flags can also be *unset* by the gateway at runtime. A 400 whose body names a request field means the gateway does not implement that field, not that the request is malformed: the field is dropped and the request retried, bounded by `MAX_FEATURE_DOWNGRADES`. Failing a whole run over an unsupported optimisation is worse than running without it, and the retry does not consume an attempt because the request that failed was never one the gateway could have served. A 400 that names no known field stays fatal.

`strict_tools` additionally requires the model's capability table to declare `supports_strict_schema`: the gateway must accept the field *and* the model must honour the grammar, so either alone is not enough.

**No stall watchdog on the stream.** The transport sets a per-read timeout (60 s) and nothing shorter, deliberately: Anthropic streams tool input as partial JSON one key at a time with multi-second gaps between them, so a short inactivity watchdog would abort a healthy stream. The bound that matters is the per-read timeout, not a gap detector.

### Prompt layout rule (cache-first)

Provider cache order is **`tools` → `system` → `messages`** — cumulative hash to the breakpoint; a change at any level invalidates that level and everything after it (`05` §KV-cache-friendly prompt construction, which owns this rule). Fixed order, append-only:

1. **tools array** — frozen for the session; sorted once at registration; byte-stable rendering. Breakpoint A (covers tools alone, so a system edit need not re-write them)
2. **system prompt** — identity, conventions, tool guidance, instruction chain (AGENTS.md). **No timestamps, no cwd, no git state, no session id.** Breakpoint B
3. **stable reference material** — skill index lines, project instructions. Grows only at session start. **No repo map**: an Aider-style prompt-injected map was the most expensive retrieval strategy tested and shows no positive isolated delta; query-time structural retrieval (+7.9pp causal) and plain grep/read are both better (`11`) |
4. **conversation history** — append-only, oldest first; rolling breakpoint on the last stable message
5. **volatile tail** — current user turn, fresh tool results, clock/cwd annotations

Rules: never mutate an emitted history message (edits/compaction invalidate everything after the mutation point — allowed, but priced as a full rewrite); timestamps/uptime live only in the tail; keep tool-result messages in history order; batch parallel tool results before the next request so the prefix grows monotonically. On Anthropic: breakpoints at layers 1, 2, and the last stable history message (max 4; watch the 20-block lookback). On [OI]: automatic prefix caching plus a stable `prompt_cache_key`; on GPT-5.6+ use explicit `prompt_cache_breakpoint`. On Gemini: implicit caching only for interactive sessions.

**A single stable-prefix anchor is not enough.** A cache read walks back only a bounded number of blocks from a breakpoint, so an anchor that never moves stops hitting once the history in front of it exceeds that window. `cache_breakpoints` therefore returns **two** offsets: the stable prefix (tools, then system) and a **rolling** one on the newest message, so the cached region extends as the conversation grows instead of silently missing. Breakpoints are applied right-to-left, because a marker occupies bytes and would otherwise shift the next offset.

**Deferred tool loading is cache-neutral.** Both Anthropic and [OI] designed tool search to preserve the prefix: discovered tool definitions are appended to *history* (Anthropic `tool_reference`, [OI] end-of-context injection), never spliced into the tools array. What breaks the cache is **client-side mutation of the `tools` array mid-session** — mcode never does that (`06` §Loading strategy, `05`).

### Reasoning-effort scheduling

Reasoning effort is a **per-phase** control, not a per-model constant. Measured on one harness/model/benchmark: all-`xhigh` scored **53.9%** (timeouts), all-`high` **63.6%**, and a **sandwich — high on planning, lower mid-implementation, high on verification — 66.5%**. More reasoning helps plan and verify; it hurts mid-implementation by doubling token and wall-clock cost on work that is mechanical.

mcode applies effort by loop state, not by tier alone:

| Loop state | Effort |
|---|---|
| `Plan`, `Replan` | high |
| `Act` (routine edits, tool calls) | medium |
| `Verify`, `Reflect` | high |
| `side` tier (titles, compaction, search) | low |

Caveat: one model, one benchmark, `[VENDOR]`-adjacent. Treat as a strong default to A/B on the eval suite, not a law. Providers differ in how effort is exposed (`reasoning.effort`, `output_config.effort`, adaptive thinking), so this is expressed in the capability registry (`15` §Provider abstraction) and degrades to a no-op where unsupported.

### Model-tier policy

| Tier | Use | Candidates | Effort | Notes |
|---|---|---|---|---|
| `plan` | planning, architecture, hard debugging, ambiguous specs | frontier (Opus/Sonnet-class, gpt-5.x sol/terra) | high | user-selectable; never auto-downgraded mid-task |
| `act` | default agent loop: edits, tool calls, routine coding | workhorse (Sonnet-class, gpt-5-mini class) | medium | the 90% tier; cached input dominates cost here |
| `side` | titles, commit msgs, summarizing tool results, search-query gen, compaction drafting | Haiku/nano/luna-class | low | latency-sensitive; no tools beyond read-only; failures degrade silently |
| `local` | offline/dev/privacy; side tasks by default | Qwen/GLM-class via Ollama/vLLM | n/a | never silently downgrades `plan`; opt-in |

Routing is by task class, not by token count: the agent loop tags each call (`plan`/`act`/`side`) at the call site. Summarize tool results >N tokens with `side` before inserting into history only when the summary is what future turns need — raw results stay retrievable. Track tier spend separately in the cost ledger.

## Traps

- Parsing `partial_json` fragments as JSON mid-stream — arbitrary splits, will throw. Concatenate, parse at terminal event.
- `previous_response_id` does not carry `instructions` forward — resend them or behavior silently changes.
- Retrying 429 `insufficient_quota`/spend-cap codes (OpenAI) or Anthropic tier-cap 429 (no `retry-after`) — burn attempts, change nothing.
- Double retry: official SDKs already retry twice; wrapping another loop multiplies backoff.
- Persisting reasoning items from `response.output_item.added` — OpenAI documents `encrypted_content` may be incomplete there; use `done`.
- Anthropic `budget_tokens` on Claude 4.7+ → hard 400. Gate thinking params on model generation via `Capabilities`.
- Bedrock: forgetting `toolUseId` echo, or sending Gemini-style `functionDeclarations` — both fail at runtime.
- Gemini OpenAI-compat shim ≠ full parity; test tool paths through the shim, not the native API alone.
- Unstable JSON key order or injected timestamps in the prefix → cache miss → you pay the 1.25× write instead of 0.10× read: a 12.5× swing per token.
- Trusting tiktoken for billing or for tool-call-heavy prompts — reconcile against `usage` and alert on drift.
- Streaming errors arrive after HTTP 200; code paths that assume "200 = success" lose mid-stream failures.

## Open questions

- Responses API `store:false` + encrypted reasoning vs Claude Code-style local-only history: which default for privacy-conscious users?
- Explicit OpenAI `prompt_cache_breakpoint` (gpt-6 family) — does breakpoint discipline materially beat automatic caching for agent loops? Needs measurement, not docs.
- Local-model `side`-tier adoption: is Qwen/GLM-class tool-calling reliable enough that `side` runs offline by default?
- Should mcode bill-estimate pre-flight with a bundled WASM tokenizer (BPE tables ≈ few MB) or keep heuristics? Binary-size constraint pushes toward heuristics.

## Sources

- https://docs.anthropic.com/en/docs/build-with-claude/streaming (SSE events, input_json_delta, mid-stream errors, unknown-event tolerance)
- https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching (1.25×/2× write, 0.10× read, TTLs, breakpoints)
- https://platform.claude.com/docs/en/api/errors (full error taxonomy, retry-after semantics, request IDs)
- https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompt-templates-and-variables (adaptive thinking, effort, budget_tokens deprecation)
- https://platform.claude.com/docs/en/build-with-claude/structured-outputs (output_config.format)
- https://platform.claude.com/docs/en/build-with-claude/token-counting + /en/api/messages/count_tokens
- https://developers.openai.com/api/docs/pricing (gpt-5/5.6/6 tables: cached input 0.10×, cache writes 1.25×, long-context 2×)
- https://developers.openai.com/api/docs/guides/prompt-caching + https://openai.com/index/api-prompt-caching/ (1024-token min, cached_tokens, 50%→model-dependent discount)
- https://developers.openai.com/api/reference/resources/responses (+ /streaming-events): event list, sequence_number, prompt_cache_breakpoint, usage cache_write/cached tokens, output_item.done reasoning caveat
- https://platform.openai.com/docs/api-reference/graders (reasoning.effort levels, model support matrix)
- https://developers.openai.com/api/docs/guides/structured-outputs (strict subset, all-required, additionalProperties:false, unsupported keywords)
- https://help.openai.com/en/articles/5955604 (429 vs quota codes, Retry-After, x-ratelimit headers) + /articles/6614457
- https://github.com/openai/openai-python (default 2 retries; RateLimitError/InternalServerError split)
- https://github.com/openai/openai-python/blob/main/src/openai/lib/streaming/chat/_completions.py (tool_call index accumulation)
- https://ai.google.dev/gemini-api/docs/generate-content/caching + /api/caching + /gemini-api/docs/pricing (explicit cache storage $/MTok/hr, TTL semantics)
- https://ai.google.dev/gemini-api/docs/openai (OpenAI-compat shim)
- https://ai.google.dev/gemini-api/docs/tools (functionDeclarations format)
- https://docs.aws.amazon.com/bedrock/latest/userguide/tool-use-client-side.html + converse CLI ref (toolSpec/toolUse/toolResult, toolUseId)
- https://gorilla.cs.berkeley.edu/leaderboard (BFCL v4 methodology)
- https://www.benchleader.com/benchmarks/bfcl_overall (Sep 2026 scores — third-party mirror)
- https://github.com/openai/tiktoken/issues/474 (tool-call token count discrepancy)
- https://docs.vllm.ai/en/latest/features/tool_calling/ (clients never set `strict`; malformed markup leaks)
- https://github.com/ollama/ollama/issues/7881 and https://github.com/ollama/ollama/issues/15457 (tool-call index absent, then constant zero)
- Community Haiku-vs-Sonnet reliability testing: benchlm.ai, mashblog.com, noelcabral.com (directional, not benchmark-grade)
