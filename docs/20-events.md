# Events

> TL;DR: A closed tagged union (`kind` + `seq` + `ts` + payload) dispatched synchronously on the loop thread to per-kind subscriber lists, with a depth cap and a pending queue for reentrancy; the same struct serializes to the session log via one serializer per kind, and Lua hooks cross the boundary as classic `lua_CFunction`s, never FFI callbacks.

## Why events are a primitive

The spec makes events the spine: the TUI, the session recorder, logging, and Lua hooks all consume the same stream, and the agent never talks to the terminal directly. That means the event type is load-bearing for four subsystems at once, so its representation is chosen for (a) codegen predictability, (b) a stable numeric tag for the log, and (c) cheap marshalling into Lua.

## Representation

**Closed tagged union, not `std::variant`.**

```cpp
enum class EventKind : uint16_t { UserMessage, AssistantDelta, ToolCall, ToolResult, /* … */ };

struct Event {
  EventKind kind;
  uint64_t seq;      // monotonic; log primary key + checkpoint index
  uint64_t ts_ns;    // producer timestamp
  Payload payload;   // union; strings are OWNED (moved in at post())
};
```

The closed union is chosen for **serialization stability**, not speed: `kind` is the stable integer the log keys on, and an open hierarchy would make the log schema implicit. At ~100–1000 events/sec the representation is unmeasurable (see the threading section), so performance is explicitly *not* the argument. Write handlers switch-shaped so a future C++26 `inspect` migration is mechanical.

Why not a virtual `IEvent` hierarchy: a heap allocation per event, an open hierarchy, and `dynamic_cast` smells.

**Payload lifetime is owned, not borrowed.** Producers run on worker threads; the loop consumes later. So `post()` **moves payload strings into event-owned storage** — no `string_view` into producer memory ever crosses a thread boundary. This is the boring, correct choice: at ~100–1000 events/sec the copy is unmeasurable, and the alternative (views into session-owned append-only buffers) requires proving the buffer is never resized while any event referencing it is alive — a use-after-free waiting to happen. Test under TSan.

## Dispatch

**Per-kind subscriber lists, not one global list with a switch.** A single list where every subscriber filters on kind makes every subscriber pay for every event type.

| Aspect | Decision | Evidence |
|---|---|---|
| Storage | `std::vector<Sub>` indexed by `EventKind` (dense enum → plain array) | Neovim's autocmd registry is a contiguous vector per event; VS Code uses per-emitter typed events |
| Ordering | Registration order | Universal default (Neovim, WezTerm, mpv) |
| Function type | `std::move_only_function` | Removes the copyability requirement `std::function` imposes; no copy tax |
| Allocation | Subscribe once, pass the event by `const&` | Never construct a `std::function` per publish |
| Optimization | Fast path for zero/one subscriber | VS Code's `Emitter` uses a single slot until the second listener, then upgrades and compacts |
| Unsubscribe | O(1) swap-remove + tombstone | Unsubscribing mid-dispatch must not invalidate iteration |

```cpp
using HookResult = std::expected<Veto, error>;   // veto only for Pre* kinds

struct EventBus {
  void publish(const Event& e);                       // sync, loop thread only
  SubId subscribe(EventKind, std::move_only_function<void(const Event&)>);
  SubId subscribe_pre(EventKind, std::move_only_function<HookResult(const Event&)>);
};
```

## Delivery semantics

Synchronous, in-loop, registration order.

| Concern | Rule |
|---|---|
| Reentrancy | Depth cap of 10 (Neovim's `E218` number) plus an `in_dispatch` flag. Events emitted from inside a hook go to a **pending deque** drained after the current dispatch completes — flat, never recursive |
| Iterator safety | Dispatch iterates a **snapshot**; register/deregister during dispatch is deferred |
| Veto | Only `Pre*` kinds (`tool.pre_call`, `spawn.pre`, `prompt.pre`). Return `expected<Veto>`; **first veto wins and short-circuits** — later handlers for that event are skipped this dispatch (`18` §Events and hooks). Non-veto subscriber errors never abort dispatch |
| Exceptions | Dispatch is `noexcept` at the bus boundary. A throwing subscriber is caught, converted to an error event + log, and **not** auto-removed |
| Backpressure | **Block the loop on the log flush; never drop.** The log is the source of truth — dropping events corrupts replay and crash recovery |
| Ordering guarantee | Single producer + sync dispatch = total order for free. State ordering guarantees per event pair, not globally |

The pending-deque approach is simpler than Neovim's nested-flag machinery (`autocmd_busy`, per-event suppression sets, `block_autocmds`) and prevents infinite recursion by construction rather than by convention.

## Threading

Tool workers and the provider stream thread produce events; the loop thread consumes them.

**Mutex + deque + condition variable.** Not lock-free.

At the measured rates this matters: mcode's realistic peak is ~100–1000 events/sec. Every candidate queue — mutex+deque, moodycamel, SPSC rings — is ≥1M items/sec at relevant thread counts, so we are 4–5 orders of magnitude below where the choice differentiates. And the numbers favor the boring option at low contention: mutex+deque at **7.75M items/s (129 ns/op)** beats moodycamel's 6.79M (147 ns/op) with 100 ns think-time, because an uncontended `std::mutex` is a single atomic CAS on the futex word with no syscall.

Lock-free only wins tail latency (SPSC p99 126 µs vs mutex p99 661 µs) and saturated multi-producer throughput — neither of which we have. It also costs TSan discipline and ARM memory-ordering correctness risk.

Neovim's own cross-thread handoff is `uv_mutex_lock` + queue + `uv_async_send`. The most-used terminal editor in the world does not use a lock-free queue for this.

Producers call `bus.post(Event)`: lock, push, notify, unlock. Bounded at ~4096 pending with block-on-full (backpressure to the producer, never a drop).

**Never call into Lua from a worker thread.** LuaJIT states are not thread-safe. All hook dispatch happens on the loop thread after the queue drain.

## Log coupling

The same event stream feeds the append-only JSONL session log. Keeping the in-memory type and the serialized form in sync without duplication:

- **One serializer per kind, adjacent to the kind's definition** — same file, same struct. `to_json(const Event&, yyjson_mut_doc*)` and `from_json`. A `constexpr` table maps `EventKind` ↔ name string ↔ schema version.
- A unit test round-trips every kind and asserts the name table is **append-only** against a golden file. This is the only sync mechanism that survives refactoring; codegen is overkill for ~15 kinds.
- Envelope: `{"v":1,"seq":N,"ts":<ns>,"kind":"tool_result","run":…,"turn":…,"step":…,"payload":{…}}`. `v` bumps only on a breaking envelope change; kinds are additive forever.
- **Unknown kinds are skipped with a warning, never fatal** — the forward-compatibility convention LSP uses for `$/`-prefixed notifications and Anthropic uses for unknown SSE event types.
- Payloads reference artifacts by path + hash (`ArtifactRef`); the log never inlines bulk bytes. This caps JSONL line size and keeps replay cheap.
- Large sessions get a periodic `Snapshot` event kind (event index + state hash) rather than a separate index file. The trigger threshold is measured, not guessed.

The OpenTelemetry logs model validates the split: fixed envelope fields for what every event shares (timestamp, severity, sequence), typed body plus attributes per event type.

## Lua hook bridging

**Classic C API, on the loop thread.** Not FFI callbacks.

| Aspect | Decision | Reason |
|---|---|---|
| Registration | `luaL_ref` the hook function into the registry | Cheap; avoids repeated `lua_getglobal` |
| Delivery | Push a pre-sized table (`lua_createtable(L, 0, 6)`) + `lua_pcall` with a `luaL_traceback` message handler | Standard, debuggable, versionable |
| Veto | Hook returns `{veto = "reason"}` (bare `false` accepted as sugar) | Explicit; carries the reason that gets surfaced to the model; anything else passes |
| Hook error | Log + pass (fail-open for capability) | Config may make `Pre*` hooks fail-closed |
| Thread | Loop thread only | LuaJIT states are not thread-safe |
| Cost | ~1 table alloc + N pushes + 1 pcall per hooked event | Microseconds at ≤100 hooked events/sec |

LuaJIT's docs are explicit that callbacks are slow ("neither the C compiler nor LuaJIT can inline or optimize across the language barrier") and recommend pull-style APIs over push-style. That warning bites at millions of calls per second; at our rate the cost is noise. It does imply two things: **do not batch or coalesce Lua hook delivery** until measurement says otherwise, and **do expose a pull-style `mcode.session_events(i)`** for extensions that want to scan history rather than react to it.

FFI callbacks are rejected outright: a hard cap of 500–1000 simultaneous callbacks, permanent callbacks created by implicit conversion, and a documented risk of the VM panicking with `"bad callback"` when a JIT-compiled C call re-enters Lua.

## Anti-patterns

| Anti-pattern | Guard |
|---|---|
| God-event union with every field optional | Closed enum + per-kind payload struct; the type system enforces which fields exist |
| Stringly-typed in-process events | `EventKind` enum; strings only at the serialization boundary |
| Bus as hidden control flow | Veto is limited to three `Pre*` kinds; everything else is a notification |
| Subscribers mutating shared state | Handlers receive `const Event&`; mutations go through the session API |
| Per-dispatch `std::function` allocation | Subscribers stored once; publish passes `const&` |
| Lock-free queues at low rates | Mutex + deque; revisit only with measurements showing contention |
| Dropping events under load | Block the producer; the log must stay complete |
| Recursive event emission | Depth cap + pending deque |

## Open questions

- Snapshot trigger threshold for long sessions — measure, don't guess.
- Is the depth cap of 10 right for hook chains, or should it be per-event-kind?
- Do we need a "tap-all" subscriber for the log writer, or should the log writer be called directly by the loop rather than subscribing?
- Should `AssistantDelta` events be coalesced before hitting subscribers (the TUI wants ~60 Hz, the log wants every delta)? Currently: log everything, coalesce only at the renderer.

## Sources

- https://pbackus.github.io/blog/beating-stdvisit-without-really-trying.html — `std::visit` indirect-call disassembly
- https://open-std.org/jtc1/sc22/wg21/docs/papers/2025/p2688r5.html — P2688R5 pattern matching status
- https://raw.githubusercontent.com/neovim/neovim/master/src/nvim/event/defs.h — function-pointer event representation
- https://raw.githubusercontent.com/neovim/neovim/master/src/nvim/autocmd.c — per-event vectors, nesting counter, `autocmd_busy`, one-shot handling
- https://raw.githubusercontent.com/neovim/neovim/master/src/nvim/loop.c — mutex + multiqueue cross-thread handoff
- https://raw.githubusercontent.com/microsoft/vscode/main/src/vs/base/common/event.ts — single-listener fast path, `onListenerError`, delivery queue, `PauseableEmitter`
- https://moderncpp.dev/articles/lock-free-queue-comparison/ — mutex vs lock-free throughput and tail latency
- https://bearcats.nl/simple-message-queue/ — queue implementations, `atomic_wait` mapping
- https://opentelemetry.io/docs/specs/otel/logs/data-model/ — envelope vs attributes split
- https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/ — ordering guarantees, `$/` forward compatibility
- https://raw.githubusercontent.com/LuaJIT/LuaJIT/v2.1/doc/ext_ffi_semantics.html — callback cost, callback cap, "bad callback" panic
- https://raw.githubusercontent.com/LuaJIT/LuaJIT/v2.1/doc/extensions.html — C++ exception interop
- https://luajit.org/ext_c_api.html — `luaJIT_setmode` wrapper
- https://www.lua.org/manual/5.4/manual.html — `luaL_ref`, `lua_createtable`, GC modes, `LUA_MINSTACK`
- https://raw.githubusercontent.com/mpv-player/mpv/master/DOCS/man/lua.rst — event registration and coalescing
- https://wezterm.org/config/lua/wezterm/on.html — ordered callbacks, `false` short-circuit
