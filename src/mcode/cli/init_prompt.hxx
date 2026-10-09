#pragma once

#include <string_view>

// The prompt `/init` submits. The command writes nothing itself: the file is
// generated from what is actually in the repository, which is the whole
// difference between this and a scaffold with empty sections.
//
// Separate from `slash.cxx` because it is data, not logic -- 139 lines of it --
// and every rule in it is a measured finding rather than taste, so it is read
// and revised on its own. The sources are in `docs/42-agents-md-research.md`.
namespace mcode::cli::detail {

	// Every rule below is a measured finding, not taste. The sources:
	//
	//  - Size dominates. Anthropic's target is under 200 lines per instruction
	//    file because "longer files consume more context and reduce adherence";
	//    IFScale measured the best frontier models at 68% adherence at 500
	//    instructions, degrading all of them uniformly rather than just the
	//    newest. A study of 100 popular repositories found Context Bloat in 42
	//    and a worst case of 1,477 lines across 27 sections.
	//  - Negative constraints beat positive directives. A 5,000-run study
	//    found every individually beneficial rule was a negative constraint
	//    and every individually harmful one a positive directive.
	//  - Lint Leakage is the most common smell (62% of files): restating what
	//    a formatter already enforces. Skill Leakage is 35%: rare procedures
	//    that belong in an on-demand skill.
	//  - Repository overviews are the one content class measured as *not*
	//    helpful, while adding over 20% inference cost.
	//  - The payoff is efficiency, not correctness: a paired 124-PR study
	//    measured median runtime -28.6% and output tokens -16.6%, with no
	//    significant correctness change.
	//  - Generated files fossilize. 24 of 100 sampled files had never been
	//    edited after generation, which is why the prompt demands evidence and
	//    why `/init` refuses to overwrite an existing file.
	//
	// A raw string with a custom delimiter, because the prompt quotes phrases
	// containing `)"`.
	inline constexpr std::string_view INIT_PROMPT = R"AGENTS(Write an AGENTS.md for this repository.

AGENTS.md is the instruction file a coding agent loads on EVERY request. It is
not documentation and not a README. Its measurable payoff is fewer wasted steps
-- a paired study of 124 pull requests found it cuts median runtime by 28.6% and
output tokens by 16.6%. Its measurable cost is context: every line is paid for
on every request and dilutes the lines around it. Length is the dominant
variable in whether the file works at all.

So this is a subtraction exercise. The hard part is not finding things to say --
it is leaving out what the repository already says.

STEP 1 -- Explore. Do not write anything yet.

Read the files that define how this project actually runs:
- The build manifest: CMakeLists.txt, package.json, Cargo.toml, pyproject.toml,
  Makefile, go.mod, build.gradle, or whatever this repository uses.
- The CI workflow under .github/workflows/ (or the equivalent). CI is where the
  real commands live, with the real flags.
- Any scripts/ or tools/ directory that holds a build, test or release script.
- README, CONTRIBUTING, and docs/. You need to know what is already documented
  so you can point at it instead of repeating it.
- Any existing AGENTS.md, CLAUDE.md, .cursorrules or CONTRIBUTING conventions.

STEP 2 -- Find the traps. This is where the value is.

A trap is something a competent engineer would get wrong without being told, or
would only discover by breaking it. Examples of the shape, not a checklist:
- A file that is generated and must not be edited by hand.
- Two things that must be changed together or the build breaks.
- A test that needs a service, a fixture or a specific order to pass.
- A flag that must never be passed outside local development.
- A directory whose name does not mean what it looks like.

Look for these in the code, in CI, in comments near the fragile thing, and in
git log for reverts and fixes. A trap you cannot point at evidence for is not a
trap -- leave it out.

STEP 3 -- Write AGENTS.md at the repository root with the write tool.

Structure, in this order. Omit any section you cannot fill from evidence:

1. Title and ONE sentence saying what the project is. One sentence, not a
   paragraph and not an overview -- repository overviews are the one content
   class measured as unhelpful, while adding over 20% inference cost.

2. Commands. The most valuable block in the file. Exact invocations WITH their
   flags: how to build, how to run the full test suite, how to run a single
   test, how to lint, how to format. `pytest` is nearly useless; the exact
   invocation the CI runs is what saves a round trip. Copy them from the
   manifest or CI, not from memory.

3. What "done" means. The exact command or exit code that proves a change
   works. An agent that cannot verify reports "looks done" and makes you the
   verification loop.

4. Constraints and traps, phrased as things NOT to do. "Do not edit
   generated/foo.cxx by hand" beats "prefer to modify the source template".
   This is the strongest measured finding in the whole document: negative
   constraints help, positive directives actively hurt. Write "do not X" and
   give the reason when the reason is not obvious.

5. Boundaries, as three short lists: always, ask first, never. Never-commit
   secrets, never force-push, never edit vendored code -- whatever is true here.

6. What to do when blocked. Without this an agent invents workarounds: deleting
   lock files, skipping hooks, ignoring a failing test. One line is enough --
   "if the suite fails twice after a fix, stop and report the failure".

7. Pointers to deeper docs, each with what it contains and when to read it. A
   bare path is ignored or over-loaded; the reason is what makes it work.

LEAVE OUT, and this list matters more than the one above:

- Anything a formatter, linter or type checker already enforces. Indentation,
  line length, naming conventions, import order, docstring rules. These are
  the most common content in real AGENTS.md files and the least useful: a
  deterministic tool checks them, and restating them crowds out what matters.
- Anything derivable from reading the code. If the agent can find it in one
  file, it does not belong here.
- Standard language conventions. "Use const correctness in C++" is not a
  project fact.
- Generic advice: "write tests", "keep functions small", "follow the existing
  style", "be careful". True of every repository, changes nothing here.
- A file-by-file inventory or an architecture essay. Describe capabilities at
  directory granularity, or name the doc and point at it.
- Rare multi-step procedures. If it applies to one task in fifty, it belongs in
  a skill loaded on demand, not in a file loaded always.
- Any section you could not fill from evidence. An omitted section is correct.
  An invented command is worse than no file, because the reader will run it.

SIZE: aim for 100 lines or fewer. Hard ceiling 150. The whole instruction chain
-- this file plus any user or organization file -- has a 2K token budget, so a
generous file here takes the budget from the rest.

MECHANICS: Markdown headings, at most two levels. No placeholders, no TODO, no
empty headings, no commented-out templates. No emoji.

BEFORE YOU WRITE, apply this test to every line you have drafted:

  "If this line were deleted, would the agent make a mistake?"

If the answer is no, delete it. This one test removes most of what a generated
file usually contains.

If the repository is small enough that there is genuinely nothing non-obvious to
say, write the smallest useful file -- the commands and the done definition are
still worth having -- and do not pad it. In your reply, say what you could not
determine rather than guessing, and tell me which lines you were least sure of,
because I have to review this file before it is worth anything.)AGENTS";
}
