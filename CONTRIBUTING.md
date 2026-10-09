# Contributing

> TL;DR: `AGENTS.md` is the authoritative rule set for this repository, and it
> applies to humans as much as to agents. Build green before the commit, docs
> move with code, and a performance claim needs a measurement.

## What this project optimizes for

mcode competes on four things, and a change is judged against them:

1. **Native speed as a product.** No runtime, small resident set, fast cold
   start. A change that adds a runtime dependency, a background thread, or
   per-invocation work needs to say what it costs.
2. **A scripting ecosystem in a native core.** C++ provides the runtime
   primitives; Lua provides the ecosystem. New capability attaches to a registry
   or the event bus, never to the loop.
3. **Context as a budgeted resource.** Every token added to the system prompt or
   the tool schema is spent on every request, forever.
4. **Trust over raw capability.** Fail closed on authority, fail open on
   capability. A recovery path must not turn a failure into a success.

The budgets in [`docs/01-north-star.md`](docs/01-north-star.md) are hard
constraints, not aspirations. If a change cannot meet one, that is a design
discussion, not a merge.

## Ground rules

[`AGENTS.md`](AGENTS.md) is the authoritative rule set. The entries below are the
ones that most often cost a review cycle; the file is the source of truth when
they disagree.

| Rule | Why |
|---|---|
| **Docs and code move together** | A decision, budget or interface change updates the doc that owns it, in the same commit. The docs are the specification; where code and docs disagree, the docs win |
| **Build is green before the commit** | Not after. A broken commit costs every other branch a rebase |
| **Stage explicit paths** | Never `git add -A`. Unrelated working-tree changes belong to someone else |
| **Deviations go in the README** | Under §Non-obvious constraints, never only in a code comment |
| **Every claim needs a source or `[UNVERIFIED]`** | Never invent a benchmark, an API name or a URL |
| **Terse** | Tables and bullets over prose. No filler, no marketing, no restating the request |
| **Decisions are stated as decisions** | "we chose X because Y". If evidence is split, show both sides and pick one |
| **One topic per file** | No merged docs, no near-duplicates |
| **No emoji** | `✓`/`✗` are allowed in TUI mockups and nowhere else |

### C++ style

The full set is in `AGENTS.md` §Code conventions. The ones that are load-bearing
in review:

- **`.cxx` and `.hxx`**, never `.cpp` or `.hpp`. Lowercase snake_case
  identifiers, no abbreviations (`length`, not `len`).
- **Tabs** for indentation, a blank line between control-flow blocks, and a space
  inside every paren pair: `fn( arg )`, `if ( x )`.
- **Trailing return types everywhere**, including `-> void`.
- **No magic numbers.** Every limit, offset, flag and status is a named
  `inline constexpr`.
- **Comments default to none.** A comment is justified only when a reader would
  otherwise get the code wrong. Never restate a name or narrate the next line.
- **`std::expected` for a fallible non-throwing path**, `std::optional` for
  "absent". Never a sentinel value in the success domain.
- **≤ 600 lines per file.** A header exists only when two or more consumers need
  it.
- **Ownership is explicit.** Name which object is charged for every resource with
  a lifetime, and which path releases it.
- **No silent-skip branch.** No empty `else`, no path that leaves a caller
  waiting forever, no empty `catch`.

`.clang-format` and `.clang-tidy` encode most of the mechanical parts; the
formatting is not a matter of taste in review.

## Setup

Requires a C++23 compiler and [Conan 2](https://conan.io/). The bootstrap script
does the three things that are easy to get wrong by hand: it activates the MSVC
developer environment, builds the private Luau package, and selects the right
Conan profile.

**Windows**

```powershell
python -m pip install "conan==2.32.0"
pwsh -File scripts/bootstrap.ps1
cmake --build build/Release
ctest --preset windows-msvc
```

**Linux / macOS**

```bash
python3 -m pip install "conan==2.32.0"
./scripts/bootstrap.sh
cmake --build build/Release
ctest --preset linux-gcc        # or macos-clang
```

> [!NOTE]
> **MSVC is only on `PATH` inside a developer prompt.** Either run
> `scripts/bootstrap.ps1` (which activates it) or start a "Developer PowerShell
> for VS". A MinGW `link.exe` on the same machine will shadow the linker and
> produce confusing errors.

A new `.cxx` file must be added to the explicit list in `src/CMakeLists.txt`, and
a new test to `tests/CMakeLists.txt`. Neither is globbed, deliberately.

## Before you push

CI is three platforms and roughly ten minutes. Run what is cheap locally first.

```bash
python _clgate.py                     # clang++ with the CI Clang warning set
./build/Release/bin/mcode --smoke     # exit code is the number of failed checks
```

`_clgate.py` compiles every translation unit with `clang++` against the real
Conan include paths from `compile_commands.json`, and reports in seconds what CI
reports in ten minutes. It catches the whole `-Wshadow` / `-Wsign-compare` /
`-Wconversion` / `-Wunused` class before you push.

> [!WARNING]
> **Two things CI catches that no local run will.**
>
> The local build defines `_WIN32`, so the POSIX branch of every platform
> conditional is never compiled on a Windows workstation. A change to Linux or
> macOS code cannot be verified locally; say so in the pull request rather than
> assuming it works.
>
> The local build does not set `MCODE_WARNINGS_AS_ERRORS`, and CI does. A
> warning that MSVC tolerates locally will fail the build. Configure with
> `-DMCODE_WARNINGS_AS_ERRORS=ON` if you want to match CI.

## Tests

```bash
ctest --preset windows-msvc --output-on-failure   # or linux-gcc / macos-clang
python tools/tui_check/tui_screen.py --exe build/Release/bin/mcode.exe
```

A permanent test must catch a plausible consumer-visible bug: behavior,
boundaries, invariants, state transitions, precedence, or error handling. It must
be deterministic, isolated, and safe in the full suite.

Do **not** add a test for any of the following. Use a throwaway script instead,
and say in the pull request what you ran:

- wiring, forwarding, or a mock echoing back what it was given
- source text, a comment, or an incidental default
- a tautology, or a bare "does not throw"
- a value that only grows, or a duplicate of a row on the same path

Existing tests that pin wording, implementation detail, or incidental behavior
are deleted rather than updated. If a test's only justification is that the code
currently does it that way, it is not a test.

`mcode --smoke` is the toolchain gate: every dependency, the Lua boundary, the
workspace boundary, and the agent loop. Its exit code is the number of failed
checks, so CI gates on it directly. Add a check when you add a subsystem, not
when you add a line.

## Commits

One logical change per commit. The subject says what the commit does; the body
says why, and what proves it.

```text
Stop relabelling the user's whole temp tree on every command

Every `bash` call took ~20.5 seconds to start. Measured on a real
app-building run: 13 bash calls, 238.8 s of a 437 s session, mean
18.4 s each, for commands that run in milliseconds.

`make_sandbox_spawn_state` calls `sandbox_windows_mark_write_paths`,
which on Windows sets an integrity label. A label propagates to every
entry beneath the path, so labelling the user's `%TEMP%` walked and
relabelled their entire temp tree on every single command.

The child now gets a temp directory of its own under the system temp,
named with the process id, created once per process.

Verified: a bash call is 23 ms end to end (was 20,541 ms). 638 test
cases pass, smoke is 149 checks with 0 failures.
```

- **Imperative mood, sentence case, no prefix.** No `feat:` or `fix:`.
- **Subject under ~72 characters**, describing the change rather than the file.
- **Body explains why**, with the measurement, the failing input, or the log line
  that proves it. `--` for a dash, and backticks around identifiers.
- **A `Verified:` line** naming what you actually ran. If you could not verify
  something, say that instead.
- **No emoji, no "Co-authored-by" trailers** unless a co-author genuinely wrote
  part of the change.

## Pull requests

A pull request should be reviewable as a self-contained argument. Fill in the
template: what changed, why, and how you verified it.

- **Small and single-purpose.** A refactor and a behavior change in one pull
  request cannot be reviewed together.
- **Describe what you could not verify.** A POSIX-only change on a Windows
  workstation, an interactive path with no test, a timing claim with one sample.
  This is the single most useful thing a pull request can say.
- **Delete dead weight in the same change.** If your change makes code, a
  parameter, or a doc section obsolete, remove it. No compatibility shims, no
  re-exports, no deprecated aliases.
- **No scope creep.** A review will not ask you to add retries, validation,
  telemetry, or an abstraction "while you are here". Do not add them unasked.

CI must be green on all three platforms. A red platform is a real failure until
proven otherwise; "it passes locally" is not evidence, because the local build
does not compile the other platforms.

## Docs

`docs/` is the specification, not commentary. It has 43 files with a fixed
structure:

- **Research docs** (`02`, `04`–`15`) are ≤ 250 lines and follow
  `# Title` → `> TL;DR` → `## State of the field` → `## What works / what doesn't`
  → `## Recommended design for mcode` → `## Traps` → `## Open questions` →
  `## Sources`.
- **Design and synthesis docs** follow `# Title` → `> TL;DR` → `## Sources`, plus
  whatever the content needs.
- `## Sources` lists only URLs actually retrieved. Label vendor claims
  `[VENDOR]`.

Changing a **decision** means updating the doc, `docs/00-index.md`, and every doc
that references it. Changing a **budget** means updating
`docs/01-north-star.md` first, because it is the single source of truth.

Never leave a stale claim. If two docs contradict, fix both or document the
disagreement explicitly in the doc that owns the decision.

## Reporting

- **Bugs and feature requests**: use the issue templates. A bug report without
  `mcode --version` and the platform is missing the two facts that most often
  explain it.
- **Security**: do not open an issue. See [`SECURITY.md`](SECURITY.md).
- **Conduct**: see [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).

## License

mcode is Apache-2.0 (see [`LICENSE`](LICENSE)). By submitting a contribution you
agree to license it under the same terms, per section 5 of that license. There is
no CLA and no copyright assignment.

`THIRD-PARTY-NOTICES.md` carries the attribution for everything mcode embeds. A
change that adds a dependency updates it, and `docs/14-cpp23-stack.md` records
why that dependency earned its size.
