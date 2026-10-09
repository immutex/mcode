## What this changes

<!-- One paragraph. What is different after this, in behaviour rather than in files. -->

## Why

<!--
The reason, not the mechanism. If this fixes a bug, what was the failing input?
If it changes a decision, which doc owns that decision and what did it say before?
-->

## Verification

<!--
What you actually ran, and what it printed. Paste the real output rather than
summarising it. `Verified:` lines belong here and in the commit message.

    ctest --preset windows-msvc        -> 638 test cases, 4244 assertions, all passed
    mcode --smoke                      -> 149 checks, 0 failures
    python _clgate.py                  -> exit 0, 0 diagnostics
-->

```text

```

## What I could not verify

<!--
**The most useful section in this template. Do not delete it.**

The local build defines `_WIN32`, so the POSIX branch of every platform
conditional is never compiled on a Windows workstation. An interactive path with
no test cannot be verified at all. A timing claim with one sample is one sample.

Write "nothing" only if you genuinely covered every platform and path this
touches. "It passes locally" is not evidence for a platform you did not build.
-->

- [ ] Nothing: every platform and path this touches was exercised
- [ ] Not verified, and here is what and why:

## Checklist

- [ ] **Build is green** — configured and built, not assumed
- [ ] **Docs move with code** — a decision, budget, or interface change updates the doc that owns it
- [ ] **New `.cxx` added to the explicit list** in `src/CMakeLists.txt` (it is not globbed)
- [ ] **New test added to** `tests/CMakeLists.txt` (also not globbed)
- [ ] **Tests catch a plausible bug** — behaviour, boundaries, invariants, or errors. No tautologies, no wiring tests, no "does not throw"
- [ ] **Dead weight removed** — code this makes obsolete is deleted, with no shims or re-exports
- [ ] **No scope creep** — no retries, validation, telemetry, or abstraction that was not asked for
- [ ] **Files stay under 600 lines**
- [ ] **A new dependency updates** `THIRD-PARTY-NOTICES.md` and justifies its size in `docs/14-cpp23-stack.md`

## Related

<!-- Fixes #, or the doc this implements. -->
