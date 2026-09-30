# Workstream A — The interactive surface

> TL;DR: `mcode` is a batch tool. Every invocation runs one task and exits, so
> there is no way to ask a follow-up, correct a wrong turn, or see anything
> while it happens. This slice builds the interactive session: a raw-mode
> terminal layer, a diffed cell renderer, streaming markdown, tool-call rows, a
> multi-line editor, an in-UI approval prompt, and a REPL that runs consecutive
> turns in one process with the history intact. **No TUI library** — `13` chose
> a custom ANSI renderer and estimated 3–5k LOC, and that estimate is the scope.

## Why this slice

Measured against the tree:

| Claim | Reality |
|---|---|
| There is an interactive mode | **There is not.** `main.cxx` dispatches `exec`, `skill`, `eval`, `--help`, and otherwise prints usage |
| The agent can ask the user something | `handle_ask_user` returns `"cannot ask the user in headless mode"` — and every run is headless |
| The user can see progress | `streaming_client` in `cli_commands.cxx` mirrors text deltas to stdout as they arrive. That is the entire presentation layer |
| The approval prompt is usable | `terminal_approval_source` prints to stdout and reads `std::cin`. It works, and it fights whatever is already on screen |
| Output is rendered | Raw text. No markdown, no diff tinting, no syntax highlighting, no folding |
| The terminal is configured | Never. No raw mode, no resize handling, no capability detection, no alternate screen |

So the harness's only interface is a command line and a wall of text. Everything
downstream of that — the permission prompt, the skill index, the MCP tools —
is reachable only by typing a fresh command with no memory of the last one.

## Scope

**In**

- A `tui/` module: terminal layer, cell buffer, frame builder, diff emitter
- Raw mode with an RAII restore guard, resize via the existing `ResizeSource`
  seam, capability detection at startup
- A hybrid inline layout: committed transcript in the terminal's own
  scrollback, a live region of ≤6 rows redrawn by diff
- Streaming markdown (headings, fences, lists, bold/italic, inline code)
- Diff rendering with line tinting and word-level emphasis on paired lines
- One live row per active tool call, collapsing to one committed line
- A multi-line input editor: history, bracketed paste, ghost-text autosuggest
- An `approval_source` implementation that prompts **inside the UI**
- `mcode` with no subcommand starts the session; `mcode exec` is unchanged
- The REPL: consecutive turns in one process, history carried across them

**Out**

- Vim mode, side-by-side diffs, mouse selection, image protocols
- A tree-sitter dependency. Fenced blocks are highlighted only if a grammar is
  already loaded; otherwise plain with dimmed punctuation
- Session persistence. The REPL's history is in memory; resume is M3
- `/reload`, slash commands, a command palette beyond Ctrl+K's binding list
- Any change to `perm/`, `ext/`, `mcp/` or the loop's state machine

## Design

### The one architectural decision to make first

`agent_loop::run( task )` is blocking and runs to a terminal state. A renderer
needs to update while it runs. There are two ways, and **this is the first
thing to settle, before any rendering work**:

1. **The loop runs on a worker thread**; the render loop owns the main thread,
   drains the event queue, and repaints. Needs the loop's event publishing to be
   thread-safe — `events::bus` already queues and dispatches synchronously, so
   this is a question about *who* calls `publish`.
2. **The loop pumps the renderer** through a callback, single-threaded.

**Prefer (1)** if the bus tolerates a publishing thread, because it keeps the
renderer's timing independent of the loop's work — a slow tool call must not
freeze the spinner. Verify the bus's threading contract in `events/bus.hxx`
before committing to it, and report which you chose and why.

### Layering

```
agent events ──┐
tool progress ─┼─► event queue (mutex + deque) ─► render coordinator
raw input ─────┘                                        │
                                                        ▼
                                            frame builder (state → cells)
                                                        │
                                        cell buffer A ◄─► cell buffer B
                                                        │
                                              diff ─► ANSI emitter ─► tty
```

`frame_builder` is a **pure function** from a state struct to a cell buffer.
That is what makes the renderer testable without a terminal: build a state,
build the frame, assert on cells — and separately assert on the emitted bytes
for a given buffer pair.

### The terminal layer

The only part that cannot be faked, so it is built first and kept small.

- Raw mode: `ENABLE_VIRTUAL_TERMINAL_PROCESSING` on Windows; `termios` on POSIX.
  **An RAII guard restores on every exit path, including a throw** — a terminal
  left in raw mode after a crash is the worst failure this slice can have.
- Resize: through the existing `platform` seam (`ResizeSource`), never a
  platform branch here.
- Input: incremental UTF-8 decode → VT parser → key/paste/mouse events. A
  partial escape sequence at a read boundary must buffer, not misparse.
- Capability probe at startup: truecolor via `COLORTERM`/`TERM`, Kitty keyboard
  with a timeout, DA1. Cache the result for the session; `NO_COLOR` and
  `MCODE_TUI` override.

### The renderer

- **Hybrid inline**, not fullscreen. Committed transcript goes to the
  terminal's own scrollback, so `SIGKILL` loses at most the live region and
  copy-paste works the way a user expects.
- Two cell buffers, swapped after flush. Diff row-granular first, then
  cell-level within changed rows.
- The live region is ≤6 rows. Everything above it is already committed.
- Wrap emission in `CSI ? 2026 h … l` when supported, so partial frames never
  tear.
- **Zero writes when nothing changed** — the render loop is event-driven, and
  the spinner is the only timer (80–120 ms, skipped when idle).

### Rendering rules that are easy to get wrong

- **Every string goes through one width function**: grapheme-segment → cluster
  width → truncate on cluster boundaries. Never mid-cluster, never a `strlen`.
  East-Asian width is ambiguous and configurable (`MCODE_AMBIGUOUS_WIDTH`).
- **No emoji in chrome.** Status glyphs are restricted to single-width
  characters (`✓ ✗ ⠿ ▸ ⋯ │`) with no variation-selector dependency — the
  emoji-width trap is exactly the kind of thing that renders perfectly on the
  author's terminal and destroys everyone else's layout.
- **Colour depth is chosen once per session**, never downsampled per cell.
  With no colour at all, fall back to attributes only.
- Theme is a table of named tokens, each with a `{truecolor, ansi256, ansi16}`
  triple. **Ship the token names and the fallback rule; choose values against a
  real terminal.**

### Markdown and diffs

- Markdown is chunk-oriented: only the last open block re-renders per token
  batch; closed blocks are immutable and committed. Never parse synchronously
  on the render thread.
- Diffs: line-level tinting; for paired `-`/`+` lines within a threshold, run a
  token LCS and emphasise only the changed tokens. Added lines highlighted,
  removed lines plain. Fold unchanged hunks of ≥3 lines.
- Tool calls: one live row (`spinner + verb + target + elapsed`), collapsing to
  `✓ edit src/x.cxx +12 −3 1.2s`. The status line carries the meter:
  `model ▸ 12.4k tok ▸ $0.031 ▸ 4.2s`. **No progress bars** — a token stream
  has no known total, so show counts.

### The approval prompt, inside the UI

`perm::approval_source` is a virtual interface with three implementations
already (terminal, headless, scripted). This slice adds a fourth that renders a
dialog in the live region and reads its answer from the input layer. It
**includes** `perm/approval.hxx` and never edits it — the interface was built
for exactly this.

The prompt must keep the semantics the engine depends on: `[?]` calls the
detail callback and re-prompts, an unrecognised answer re-prompts, and a closed
input is `refused` (which the engine resolves as deny). A prompt that defaults
is a prompt that grants by accident.

### The REPL

- `mcode` with no arguments enters the session; `mcode exec` keeps its current
  contract exactly, because CI and the eval suite depend on it.
- Consecutive turns in one process, history carried across them. A follow-up
  sees the previous turn's tool results.
- **The remember store must survive across turns** — answering "always" in turn
  one must not re-prompt in turn two. That is the intersection with the second
  batch's engine and the batch-level acceptance test.
- Ctrl+C interrupts the current turn and returns to the prompt; it does not
  exit. Ctrl+D at an empty prompt exits.
- Exit codes stay meaningful: a session ends with the last turn's code.

## Files

| Path | Owner | Change |
|---|---|---|
| `src/mcode/tui/tty.{hxx,cxx}` | A | raw mode guard, capability probe, resize, input decode |
| `src/mcode/tui/cell.{hxx,cxx}` | A | `cell`, `style`, `cell_buffer`, the width function |
| `src/mcode/tui/frame.{hxx,cxx}` | A | pure `state → buffer`, diff, ANSI emit |
| `src/mcode/tui/theme.{hxx,cxx}` | A | named tokens and the depth fallback rule |
| `src/mcode/tui/markdown.{hxx,cxx}` | A | incremental block parser and renderer |
| `src/mcode/tui/diff_view.{hxx,cxx}` | A | line tinting, token LCS, folding |
| `src/mcode/tui/editor.{hxx,cxx}` | A | multi-line input, history, paste, autosuggest |
| `src/mcode/tui/approval_tui.{hxx,cxx}` | A | `perm::approval_source` over the UI |
| `src/mcode/tui/render.{hxx,cxx}` | A | the render coordinator and event queue |
| `src/mcode/cli/repl.{hxx,cxx}` | A | the session loop and turn sequencing |
| `src/cli_commands.cxx` | **A** | the REPL entry, reusing the existing construction |
| `src/main.cxx` | **A** | the no-subcommand branch |
| `tests/test_tui_frame.cxx` | A | frame/diff/emit assertions on bytes |
| `tests/test_tui_markdown.cxx` | A | block parsing and streaming |
| `tests/test_tui_editor.cxx` | A | input handling against synthetic key events |
| `tests/test_tui_repl.cxx` | A | two turns in one process, history intact |

## Steps

1. **Settle the threading model** (above) and report the choice. Nothing else
   starts until this is decided.
2. **Verify the loop supports consecutive turns.** If `run()` cannot be called
   twice in one process — a terminal-state guard, a stale budget, history that
   resets — that is a loop change, and it is yours. Do it here, before any
   rendering.
3. `tty`: raw mode with the RAII guard, capability probe, resize through the
   seam, incremental input decode. Test the decoder and the guard; the terminal
   itself is exercised by hand.
4. `cell` + `theme`: buffers, styles, the width function. Pure, fully testable.
5. `frame`: pure builder, row-then-cell diff, ANSI emitter with SGR delta
   tracking. Assert on emitted bytes.
6. `render`: the coordinator, the queue, the commit protocol.
7. `markdown`, then `diff_view`, then `editor`.
8. `approval_tui`, then the REPL wiring in `cli_commands.cxx` and `main.cxx`.

## Acceptance

- A two-turn session in one process with history intact, and a follow-up that
  sees the first turn's tool results.
- "Always" at an approval prompt in turn one does not re-prompt in turn two.
- Ctrl+C mid-turn returns to the prompt and the session continues; Ctrl+D exits.
- `mcode exec` is byte-for-byte unchanged: same flags, same JSON lines, same
  exit codes. The eval suite and CI depend on it.
- A frame diff emits **nothing** when the state does not change, and emits only
  the changed cells when one line changes.
- The width function truncates on cluster boundaries: a test with a CJK string,
  a combining mark, and a ZWJ emoji sequence.
- Raw mode is restored on normal exit **and** on an exception path.
- `NO_COLOR` produces attributes-only output; `MCODE_TUI=inline` and
  `full` are both honoured.
- Rendering a fixed state to a buffer twice produces identical bytes —
  the renderer is deterministic, which is what makes the CI check possible.

Live evidence: the batch acceptance run in `36`.

## Traps

- **A terminal left in raw mode.** Every exit path, including a throw, must
  restore. This is the failure that makes a user reboot.
- **Blocking the render thread on the loop.** If a slow tool freezes the
  spinner, the threading model is wrong.
- **Emoji and ambiguous-width glyphs in chrome.** Single-width only; one width
  function everywhere.
- **Assuming a colour depth.** Probe, then pick once. Never per-cell.
- **Parsing markdown synchronously per token.** Only the last open block
  re-renders.
- **A prompt that defaults.** Unrecognised re-prompts; closed input is
  `refused`. The engine's fail-closed contract depends on it.
- **Changing `mcode exec`.** It is CI's and the eval suite's interface. A
  "small improvement" there breaks the only automated gate the harness has.
- **Fullscreen by default.** Inline keeps the transcript in the terminal's
  scrollback; a crash loses at most the live region.
- **Forgetting the resize path.** A stale column count produces a frame that
  is correct in width and wrong on screen.

## Sources

- `docs/13-cli-and-tui.md` — the architecture, cell buffer, commit protocol, theme tokens, glyph and width rules, and the traps list this section extends
- `docs/12-security.md` — why the approval prompt must stay fail-closed
- `docs/24-cross-platform.md` — the `ResizeSource` seam, ConPTY floor, per-platform terminal notes
- `docs/33-workstream-permissions.md` — `approval_source`, its outcomes, and the remember store the REPL must not break
- `docs/04-agent-loop.md` — the state machine and terminal states the REPL drives
- `docs/20-events.md` — the bus the renderer drains
