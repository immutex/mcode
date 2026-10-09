# AGENTS.md

mcode — C++23 coding-agent harness. Luau is the extension layer.
**Spec: `docs/00-index.md`.** Where code and docs disagree, the docs win.

## Layout

| Path | Contents |
|---|---|
| `docs/` | 41 design, research and workstream docs — the authoritative spec |
| `src/mcode/` | Library sources: `core/` `support/` `ext/` `fs/` `net/` `proc/` `agent/` |
| `src/main.cxx` | Startup smoke test; exit code = number of failed checks |
| `tests/` | Catch2 unit tests |
| `cmake/` | Warning set, per-platform target config |
| `conan/profiles/` | Per-platform Conan profiles |
| `conan/recipes/luau/` | Private Luau recipe — upstream ships CMake, so this is a thin wrapper |
| `scripts/` | `bootstrap.ps1` / `bootstrap.sh` — dev setup, builds from source |
| `install.sh`, `install.ps1` | The one-line installers: download, verify, install, hand off to `mcode setup` |
| `.github/workflows/ci.yml` | Three-platform CI matrix |

Dependencies point downward only, from `agent/` toward `core/`.
No agent behaviour yet: no model client, no tool implementations, no TUI.

## Rules

1. **Docs and code move together.** Changing a decision, budget or interface means updating the doc that owns it.
2. **Deviations go in `docs/40-non-obvious-constraints.md`** — never only in a code comment.
3. **Every claim needs a source or an explicit `[UNVERIFIED]`.** Never invent a benchmark, API name or URL.
4. **Terse.** Tables and bullets over prose. No filler, no marketing, no restating the request.
5. **Decisions are stated as decisions** — "we chose X because Y". If evidence is split, show both sides and pick one.
6. **One topic per file.** No merged docs, no near-duplicates.
7. **No emoji**, except `✓`/`✗` in TUI mockups.
8. **Build is green before the commit**, not after.
9. **Stage explicit paths.** Never `git add -A`. Never a broad `git checkout -- <list>`.
10. **No build or temp artifacts in the tree.** Scratch goes to `%TEMP%`.

### Docs

- Research docs (`02`, `04`–`15`): ≤250 lines, `# Title` → `> TL;DR` → `## State of the field` → `## What works / what doesn't` → `## Recommended design for mcode` → `## Traps` → `## Open questions` → `## Sources`.
- Design docs (`17`–`25`) and synthesis docs (`00`, `01`, `03`, `16`): `# Title` → `> TL;DR` → `## Sources`, plus whatever sections the content needs.
- `## Sources` lists only URLs actually retrieved. Label vendor claims `[VENDOR]`.
- Changing a **decision**: update the doc, `docs/00-index.md`, and every doc referencing it.
- Changing a **budget**: update `docs/01-north-star.md` first — it is the single source of truth.
- Never leave a stale claim. If two docs contradict, fix both or document the disagreement explicitly.

## Code conventions

### Files and naming

- **Extensions: `.cxx` for implementation, `.hxx` for headers.** Never `.cpp` or `.hpp`.
- Files, classes, structs, enums, methods, functions: **lowercase snake_case**. `tool_registry`, `read_file`.
- **No abbreviated identifiers.** `length`, not `len`; `offset`, not `off`; `index`, not `i`. Domain terms of art are exempt (`modrm`, `rel32`, `utf8`).
- Constants: `SCREAMING_SNAKE_CASE`, `inline constexpr`. Globals and file-scope state: `g_` prefix.
- **No magic numbers.** Every limit, offset, flag and status is a named `inline constexpr`.
- A header exists only when ≥2 consumers need it.
- ≤600 lines per file.

### Comments

- **Default is none.** A comment is justified only when a reader would otherwise get the code *wrong*.
- Never restate a name, narrate the next line, or describe the obvious.
- Never a banner, a trailing comment restating the name, or commented-out code.
- Lowercase, max one line.
- No `docs/NN` citations in code.
- **When code and comment disagree, one of them is a bug** — fix it in the same change.

### Layout

- **Tabs** for indent. Namespace bodies are indented.
- **Blank line between control-flow blocks.** `if`/`for`/`while`/`switch`/`try` are never stacked.
- `break` and `continue` hug their statement — no blank line above them. This is the one exception above.
- `switch` bodies are indented one level; blank line between case groups.
- **Trailing return types everywhere**: `auto name( args ) -> type {`, including `-> void`.
- **Space inside every paren pair**: `fn( arg )`, `fn( )`, `if ( x )`. Templates: `T< U >`.
- ≤4 positional parameters; beyond that take a request/options struct.

### C++23

- `auto` for any local initialized at its declaration.
- `std::expected` for a fallible non-throwing path; `std::optional` for "absent". Never a sentinel value in the success domain.
- `enum class` always.
- `[[nodiscard]]` on every query (const member function). Not on status-returning actions.
- `const` on every by-value parameter, except an assignment cursor, a writable-address cast, or a moved-from value.
- Structured bindings; `std::span` over pointer+length; concepts over SFINAE.
- A class owns its state. No all-public bags, no internals in a public header.

### Correctness

- **Ownership is explicit.** Name which object is charged for every resource with a lifetime, and which path releases it.
- **Make the guard actually guard.** A re-entrancy flag is test-and-set. A bounds check uses 64-bit arithmetic — a 32-bit `offset + length` wraps and passes. Check against real capacity, not a larger enclosing size.
- **No silent-skip branch.** No empty `else`, no path that leaves a caller waiting forever.
- **A recovery path must not turn a failure into a success.** Zero is not neutral when zero is the success encoding.
- **Fail closed.** Missing input, unreadable state or unexpected error hard-fails.
- **No empty `catch`.** Use `result<T>`, `std::optional`, or a named handler.
- **No exception escapes a destructor** — that is `std::terminate`, not a caught error.
- **One strategy per path.** A compatibility fallback only where the OS forces it.
- **One implementation per concept.** A duplicated parser or loader that drifts returns a wrong value.
- **Delete dead design.** Fields nothing reads or writes are removed, not carried.
- **A diagnostic set is bounded.** An ever-growing set with no eviction is a leak.
- **Unknown CLI flags are not ignored silently.**
- **Never satisfy a check with a no-op** such as `(void)0;`. Fix the cause.

### Editing

- **Count-guarded edits.** Assert the match count, then read the region back. A bare `str.replace` no-ops on a whitespace mismatch and rewrites string literals.
- **Renames are identifier-scoped.** A blind replace of a short name is a rewrite.

## Vocabulary

Use exactly, as defined in `docs/01-north-star.md`:

**Harness · Session · Turn · Step · Run · Subagent · Skill · Extension · Tool · Artifact · Compaction**

## Design rule

> **C++ provides the runtime primitives. Lua provides the ecosystem.**

The core knows four abstractions — `ModelClient`, `Context`, `ToolRegistry`, `EventBus` — and nothing about Lua, MCP, skills, or the terminal. New capability attaches to a registry or the event bus, never to the loop.

## Budgets

Defined in `docs/01-north-star.md`. Never redefine them; reference them.

| Constraint | Budget |
|---|---|
| Cold start | ≤ 15 ms |
| Idle RSS | ≤ 30 MB |
| Static binary (with TLS) | ≤ 25 MB (target ~5–9 MB) |
| Direct third-party deps | ≤ 12, with **size** as the binding constraint |
| Always-loaded tool schema | ≤ 3K tokens core (≤3.5K with default extensions) |
| Harness-added TTFT overhead | ≤ 5 ms |
| Extension load cost | O(1) in extension count |
| System prompt | ~1,200 words core (excl. tool descriptions, instruction chain) |
| Session-start context | ≤8.5K tokens: prompt ≤1.5K + tools ≤3.5K + instructions ≤2K + skill index ≤1.5K |
| Read viewport | 100 lines default (measured optimum) |
| Platform tiers | Windows, Linux, macOS — **all tier-1** |

## Commands

```powershell
pwsh -File scripts/bootstrap.ps1     # once per machine
cmake --build build/Release
ctest --preset windows-msvc
./build/Release/bin/mcode.exe        # exit code = number of failed checks
```

Linux and macOS: `./scripts/bootstrap.sh`, `ctest --preset linux-gcc` or `macos-clang`.
Strict build: `-DMCODE_WARNINGS_AS_ERRORS=ON`. Fast iteration: the `dev` preset.

MSVC is only on `PATH` inside a developer prompt. A MinGW `link.exe` on the same machine shadows the linker and produces confusing errors.

## Where to look

| Question | Doc |
|---|---|
| Decisions, reading order | `docs/00-index.md` |
| Hard constraints, vocabulary | `docs/01-north-star.md` |
| Prior art | `docs/02-prior-art.md` |
| Architecture, layers | `docs/03-architecture.md` |
| Loop and termination | `docs/04-agent-loop.md` |
| Context budgeting, compaction | `docs/05-context-engineering.md` |
| Core tool set | `docs/06-tools.md` |
| MCP client | `docs/07-mcp.md` |
| Skills and AGENTS.md | `docs/08-skills-and-agents-md.md` |
| Memory | `docs/09-memory.md` |
| Subagents | `docs/10-subagents.md` |
| Reliability, evals | `docs/11-reliability-and-evals.md` |
| Permissions, trust boundary | `docs/12-security.md` |
| Terminal UI | `docs/13-cli-and-tui.md` |
| Stack, build, libraries | `docs/14-cpp23-stack.md` |
| Providers and cost | `docs/15-model-layer.md` |
| Milestones, risks | `docs/16-roadmap.md` |
| **Current work plan — read before starting** | `docs/26-first-batch.md` |
| **Traps and deviations — cited by number** | `docs/40-non-obvious-constraints.md` |
| Extension runtime, API, lifecycle | `docs/17-lua-runtime.md` … `docs/23-first-party-extensions.md` |
| Cross-platform, packaging | `docs/24-cross-platform.md`, `docs/25-distribution.md` |
| **Contributing, style rules, CI traps** | `CONTRIBUTING.md` |
| **Security policy, what ships vs. what is designed** | `SECURITY.md` |
| **What changed in each release** | `CHANGELOG.md` |
| Third-party attribution, licenses | `THIRD-PARTY-NOTICES.md` |

Each design doc carries a `## Traps` section. Open disagreements between sources are recorded in the doc that owns the decision — read it before "fixing" one.
