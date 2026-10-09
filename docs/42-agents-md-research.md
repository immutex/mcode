# AGENTS.md research

> TL;DR — the findings that change what `/init` should generate
>
> 1. **AGENTS.md is a plain-Markdown convention, not a schema.** No required fields, no frontmatter, closest-file-wins. Stewarded by the Agentic AI Foundation (Linux Foundation); 60k+ repos contain one. ([agents.md](https://agents.md/))
> 2. **Size is the dominant variable.** Anthropic: target **under 200 lines** per instruction file; longer files "consume more context and reduce adherence". Their test for every line: *"Would removing this cause Claude to make mistakes?" If not, cut it.* Bloated files cause the agent to **ignore the instructions that matter**. ([Claude Code memory docs](https://docs.anthropic.com/en/docs/claude-code/memory), [best practices](https://www.anthropic.com/engineering/claude-code-best-practices))
> 3. **Instruction count degrades *all* instructions uniformly, not just the new ones.** IFScale measured the best frontier models at **68% accuracy at 500 instructions**; degradation is roughly linear for frontier thinking models and exponential for smaller ones, with a **bias toward earlier instructions**. HumanLayer's practical estimate: frontier models hold **~150-200 instructions** with reasonable consistency, and [CC]'s own system prompt already spends **~50** of them. ([IFScale, arXiv 2507.11538](https://arxiv.org/abs/2507.11538), [HumanLayer](https://www.humanlayer.dev/blog/writing-a-good-claude-md))
> 4. **Most of what people put in AGENTS.md is dead weight.** A study of 100 popular repos found **91/100 files contained at least one "configuration smell"**: *Lint Leakage* (62%) — restating what a linter/formatter already enforces; *Context Bloat* (42%); *Skill Leakage* (35%) — rare procedures that belong in an on-demand skill. ([Configuration Smells, arXiv 2606.15828](https://arxiv.org/html/2606.15828v2))
> 5. **Correctness is not the win condition; efficiency is.** ETH Zurich (Gloaguen et al.): context files **do not generally improve task success** and add **>20% inference cost**; repository overviews are *not* helpful. A second controlled ablation found the correctness effect bounded to <=10-15pp with the real AGENTS.md never converting a near-miss to a pass. But a 124-PR paired study found AGENTS.md **lowers median runtime by 28.6% and output tokens by 16.6%**. Write for *fewer wasted steps*, not for "better code". ([arXiv 2602.11988](https://arxiv.org/abs/2602.11988), [arXiv 2607.27250](https://arxiv.org/html/2607.27250v1), [arXiv 2601.20404](https://arxiv.org/html/2601.20404v2))
> 6. **Negative constraints beat positive directives.** A 5,000-run study on SWE-bench Verified found every individually *beneficial* rule was a **negative constraint** ("do not refactor unrelated code") and every individually *harmful* rule was a **positive directive** ("follow code style") — both curated and random rules gave +13.8pp, i.e. gains are largely content-independent priming. ([Guardrails Beat Guidance, arXiv 2604.11088](https://arxiv.org/abs/2604.11088))
> 7. **Command-first, not prose.** GitHub's analysis of 2,500+ repos: successful files put **exact executable commands early**, show one real code example instead of three paragraphs, and set explicit **boundaries** (never touch secrets/vendor/prod config). ([GitHub blog](https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/))
> 8. **The generator must be restrained, not comprehensive.** Codex's own `/init` prompt asks for **200-400 words**. Two independent guides say *never* let `/init` auto-generation stand unreviewed — generated files "prioritize comprehensiveness over restraint" and become "Init Fossilization". ([Codex init prompt, verbatim mirror](https://raw.githubusercontent.com/kekmodel/codex-system-prompts/main/prompts/tui/tui-init-command.md), [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md), [arXiv 2606.15828](https://arxiv.org/html/2606.15828v2))

---

## State of the field

**What it is.** "A simple, open format for guiding coding agents… Think of AGENTS.md as a **README for agents**: a dedicated, predictable place to provide the context and instructions to help AI coding agents work on your project." Used by 60k+ open-source projects. `[VENDOR]` — [agents.md](https://agents.md/)

**Maintainer.** Introduced 2025 by [OC], Amp, Google Jules, Cursor and Factory; now stewarded by the **Agentic AI Foundation (AAIF) under the Linux Foundation**. `[VENDOR]` — [agents.md](https://agents.md/); the adoption list on the same page includes Codex, Jules, Factory, Aider, goose, opencode, Zed, Warp, VS Code, Devin, UiPath, Junie, Cursor, Amp, RooCode, Gemini CLI, Kilo, Semgrep, Copilot, Windsurf, Augment.

**Spec content.** There is none beyond Markdown:
- *"Are there required fields? No. AGENTS.md is just standard Markdown. Use any headings you like; the agent simply parses the text you provide."* `[VENDOR]` — [agents.md FAQ](https://agents.md/)
- **Conflict rule:** *"The closest AGENTS.md to the edited file wins; explicit user chat prompts override everything."* `[VENDOR]` — [agents.md FAQ](https://agents.md/)
- **Nesting:** one AGENTS.md per subproject; agents read the nearest file in the directory tree. [OI]'s own monorepo had **88** AGENTS.md files at time of writing. `[VENDOR]` — [agents.md](https://agents.md/)
- **Migration:** `mv AGENT.md AGENTS.md && ln -s AGENTS.md AGENT.md`. `[VENDOR]` — [agents.md FAQ](https://agents.md/)

**How tools load it (this matters for what `/init` writes).**

| Tool | Load semantics | Source |
|---|---|---|
| Codex | Global `~/.codex/AGENTS.override.md` else `AGENTS.md`; then project root -> cwd, one file per dir, concatenated root-down, later = higher precedence; **stops at `project_doc_max_bytes` (32 KiB default)** | [Codex docs](https://learn.chatgpt.com/docs/agent-configuration/agents-md) `[VENDOR]` |
| [CC] | CLAUDE.md from cwd + all ancestors, concatenated root->cwd (closest read last); subdir files loaded JIT on file access; `AGENTS.md` read directly or alongside | [Claude Code memory docs](https://docs.anthropic.com/en/docs/claude-code/memory) `[VENDOR]` |
| Cursor / Amp / Gemini CLI | Nested AGENTS.md combined, "more specific takes precedence" (Cursor); Amp falls back to `AGENT.md`/`CLAUDE.md` | [agents.md](https://agents.md/), [docs/08](docs/08-skills-and-agents-md.md) `[VENDOR]` |

**Relationship to CLAUDE.md / GEMINI.md / .cursorrules.** One file can serve several tools: Codex, Cursor, Copilot, Amp, Aider, Zed, Warp, Devin, Windsurf etc. read AGENTS.md natively; [CC] prefers `CLAUDE.md` but **does read a repository's AGENTS.md** ("If your repository uses AGENTS.md instead, see AGENTS.md"). The pragmatic cross-tool pattern is **AGENTS.md as canonical + a symlink or one-line pointer** (`CLAUDE.md` containing `read AGENTS.MD`), *not* parallel hand-maintained files. `[VENDOR]` — [agents.md](https://agents.md/), [Claude Code memory docs](https://docs.anthropic.com/en/docs/claude-code/memory), [Configuration Smells §II](https://arxiv.org/html/2606.15828v2)

---

## What works

**1. Command-first, copy-pasteable.** GitHub's analysis of 2,500+ repos: *"Put relevant executable commands in an early section: `npm test`, `npm run build`, `pytest -v`. Include flags and options, not just tool names. Your agent will reference these often."* The six areas that put a file in the top tier: **commands, testing, project structure, code style, git workflow, boundaries**. `[VENDOR]` — [GitHub blog](https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/)

**2. Closure definitions ("done" is exit codes, not vibes).** Blake Crosley: *"Explicit closure definitions eliminate the most common failure mode: the agent reports 'done' without verifying. When 'done' is defined as specific exit codes, the agent runs each check before reporting completion."* His writing order: **build/test commands -> definition of done -> escalation rules -> task-organized sections -> directory scoping**. `[THIRD-PARTY]` — [blakecrosley.com](https://blakecrosley.com/blog/agents-md-patterns)

**3. Verification is the single highest-leverage content.** Anthropic: *"Give Claude a check it can run: tests, a build, a screenshot to compare… Without a check it can run, 'looks done' is the only signal available, and you become the verification loop."* `[VENDOR]` — [Claude Code best practices](https://www.anthropic.com/engineering/claude-code-best-practices)

**4. Boundaries, in three tiers.** GitHub's template uses **Always / Ask first / Never**. *"'Never commit secrets' was the most common helpful constraint."* `[VENDOR]` — [GitHub blog](https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/); Crosley adds explicit **escalation rules** ("if tests fail after 3 attempts: stop and report") because *"without escalation rules, agents default to increasingly creative workarounds… deleting lock files, bypassing checks, or silently ignoring failures."* `[THIRD-PARTY]` — [blakecrosley.com](https://blakecrosley.com/blog/agents-md-patterns)

**5. Only non-standard, non-inferable facts.** Anthropic's include/exclude table is the clearest published rule: **include** commands the agent can't guess, style that *differs from defaults*, test runners, repo etiquette, project-specific architecture, env quirks, non-self-evident gotchas; **exclude** anything derivable from code, standard language conventions, API docs (link instead), frequently-changing info, file-by-file descriptions, self-evident advice. `[VENDOR]` — [Claude Code best practices](https://www.anthropic.com/engineering/claude-code-best-practices). The empirical version: ETH Zurich found **only non-standard coding practices** were useful; **repository overviews were not**. `[THIRD-PARTY]` — [arXiv 2602.11988](https://arxiv.org/abs/2602.11988)

**6. One real example beats three paragraphs.** GitHub: *"One real code snippet showing your style beats three paragraphs describing it."* `[VENDOR]` — [GitHub blog](https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/)

**7. Progressive disclosure with pitched pointers.** Keep the always-on file tiny and point at deeper docs **with a reason and a trigger**: *"For TypeScript conventions, see docs/TYPESCRIPT.md"*. A bare path is a **Blind Reference** — the agent either ignores it or loads something huge. `[THIRD-PARTY]` — [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md), [HumanLayer](https://www.humanlayer.dev/blog/writing-a-good-claude-md), [arXiv 2606.15828 §IV-D](https://arxiv.org/html/2606.15828v2)

**8. Describe capabilities, not file paths.** *"Documentation goes out of date quickly… stale information actively poisons the context. This is especially dangerous when you document file system structure."* Domain concepts ("organization" vs "workspace") are more stable than paths. `[THIRD-PARTY]` — [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md)

**9. Real high-quality examples and their shape.** Apache Airflow's `AGENTS.md` (a top-tier real file) is ~200 lines and is organised as: **Naming / Environment Setup / Commands / Repository Structure / Architecture Boundaries / Security Model / Coding Standards / Testing Standards / Output conventions / Commits and PRs / Boundaries (Ask first / Never) / References** — commands are exact invocations with flags (`breeze testing providers-tests --test-type "Providers[google]"`), and constraints are negative and enforceable ("No `assert` in production code"; "Never add new direct `raise AirflowException(...)`"). `[THIRD-PARTY]` — [apache/airflow AGENTS.md](https://raw.githubusercontent.com/apache/airflow/main/AGENTS.md). Codex's own `/init` recommended outline: Project Structure & Module Organization; Build, Test, and Development Commands; Coding Style & Naming Conventions; Testing Guidelines; Commit & Pull Request Guidelines — *"Keep the document concise. 200-400 words is optimal."* `[VENDOR]` — [Codex init prompt (verbatim mirror)](https://raw.githubusercontent.com/kekmodel/codex-system-prompts/main/prompts/tui/tui-init-command.md)

**10. Efficiency, not correctness, is the measurable payoff.** Paired 124-PR study: AGENTS.md -> **median runtime -28.64%, median output tokens -16.58%, mean wall-clock -20.27%** (`p<0.05` for time and output tokens), with "comparable task completion behaviour". `[THIRD-PARTY]` — [arXiv 2601.20404](https://arxiv.org/html/2601.20404v2). The mechanism is visible in traces: with a runtime warning present, blind full-suite test runs fell monotonically (3.67 -> 2.44 -> 1.67 per cell). `[THIRD-PARTY]` — [arXiv 2607.27250 §4.2](https://arxiv.org/html/2607.27250v1)

---

## What doesn't

This is the more important half: the failures are measured, replicated, and quantified.

### F1 — Context bloat: longer files make the agent follow *fewer* of your instructions
- Anthropic: *"target under 200 lines per CLAUDE.md file. Longer files consume more context and reduce adherence."* And: *"If Claude keeps doing something you don't want despite having a rule against it, the file is probably too long and the rule is getting lost."* `[VENDOR]` — [Claude Code memory docs](https://docs.anthropic.com/en/docs/claude-code/memory), [best practices](https://www.anthropic.com/engineering/claude-code-best-practices)
- Measured prevalence: **Context Bloat in 42/100 files** (threshold >=200 lines); the worst file analysed was **1,477 lines across 27 sections** — a real repo's `CLAUDE.md`. `[THIRD-PARTY]` — [arXiv 2606.15828 §VI-A](https://arxiv.org/html/2606.15828v2)
- Mechanism: Chroma's *Context Rot* evaluated **18 LLMs** and found performance degrades as input length grows **even on trivially simple tasks** (repeated-words replication), and that **distractors amplify** the degradation. `[THIRD-PARTY]` — [Chroma](https://www.trychroma.com/research/context-rot)
- Position effect: *Lost in the Middle* — performance is highest when relevant information is at the **beginning or end** and *"significantly degrades when models must access relevant information in the middle of long contexts."* `[THIRD-PARTY]` — [arXiv 2307.03172](https://arxiv.org/abs/2307.03172)

### F2 — Instruction-count degradation is uniform, not local
IFScale (500 keyword-inclusion instructions, 20 SOTA models): *"even the best frontier models only achieve 68% accuracy at the max density of 500 instructions"*; degradation patterns differ by model size/reasoning; models show **"bias towards earlier instructions"**. `[THIRD-PARTY]` — [arXiv 2507.11538](https://arxiv.org/abs/2507.11538)

HumanLayer's synthesis: **frontier thinking LLMs follow ~150-200 instructions** with reasonable consistency; smaller/non-thinking models decay **exponentially** vs linear; **"as instruction count increases, instruction-following quality decreases uniformly"** — the model does not merely ignore the newest lines, it starts ignoring *all* of them. They measured **~50 individual instructions in [CC]'s system prompt** before any user rules, plugins or skills. Their root CLAUDE.md is **under sixty lines**. `[THIRD-PARTY]` — [HumanLayer](https://www.humanlayer.dev/blog/writing-a-good-claude-md)

### F3 — Lint Leakage: the most common smell (62%)
Restating what a deterministic tool already enforces — indentation, line length, naming conventions, import ordering, docstring rules. *"Because these constraints are automatically checked by local tools, repeating them in AGENTS.md adds limited value while unnecessarily increasing the agent's context size… emphasizing such coding rules can divert the model from focusing on more important project-specific concerns."* Real example: `google/adk-python`'s "Python Style Guide" section, which the maintainers later **extracted into a separate skill**. `[THIRD-PARTY]` — [arXiv 2606.15828 §IV-C, §VI-C](https://arxiv.org/html/2606.15828v2)

Corroboration: *"Never send an LLM to do a linter's job. LLMs are comparably expensive and incredibly slow compared to traditional linters… Code style guidelines will inevitably add a bunch of instructions and mostly-irrelevant code snippets into your context window, degrading your LLM's performance and instruction-following."* Use a formatter + a hook instead. `[THIRD-PARTY]` — [HumanLayer](https://www.humanlayer.dev/blog/writing-a-good-claude-md)

### F4 — Skill Leakage: rare procedures in the always-on file (35%)
Instructions useful for "a small subset of tasks" load into **every** session, "compete for attention with the rules that are actually critical", and cost tokens on every request. The most common leaked skills were **Testing (10), Workflow (8), Scaffolding (4), Infrastructure (4), Architecture (3)**. `[THIRD-PARTY]` — [arXiv 2606.15828 §IV-B, §VI-B](https://arxiv.org/html/2606.15828v2). Fix: separate markdown files / skills loaded on demand. `[THIRD-PARTY]` — [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md), [HumanLayer](https://www.humanlayer.dev/blog/writing-a-good-claude-md)

### F5 — Conflicting instructions resolve arbitrarily
- Anthropic: *"if two instructions contradict each other, Claude may pick one arbitrarily."* Their shipped remedy is a **`/doctor prompt-audit`** that finds instructions "written for older models, references to files or commands that don't exist, and files that contradict each other." `[VENDOR]` — [Claude Code memory docs](https://docs.anthropic.com/en/docs/claude-code/memory)
- Detected in **28/100 files** (16 confirmed after manual review; the LLM heuristic's precision was only 57%, itself a signal of how subtle the conflicts are). Concrete real conflict: one file placed components in `packages/ui/components` in one section and `packages/components` in another. `[THIRD-PARTY]` — [arXiv 2606.15828 §IV-F, §VI-F](https://arxiv.org/html/2606.15828v2)

### F6 — Stale content and blind references poison context
- **Init Fossilization (24/100 files):** files generated by `/init` and never touched again. Not explained by dormant projects — *"we did not find a single project in such a situation, that is, with zero commits after the creation of the AGENTS.md file."* `[THIRD-PARTY]` — [arXiv 2606.15828 §IV-E, §VI-E](https://arxiv.org/html/2606.15828v2)
- **Blind Reference (16/100):** *"If you just mention the path, Claude will often ignore it. You have to pitch the agent on why and when to read the file."* `[THIRD-PARTY]` — [arXiv 2606.15828 §IV-D](https://arxiv.org/html/2606.15828v2)
- Root cause of both: generated docs record **paths**, which change. *"If your AGENTS.md says 'authentication logic lives in src/auth/handlers.ts' and that file gets renamed or moved, the agent will confidently look in the wrong place."* `[THIRD-PARTY]` — [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md)
- Co-occurrence: **Conflicting Instructions + Skill Leakage -> Context Bloat at 83% confidence**; Conflicting Instructions alone -> Context Bloat at 81%. Smells cascade. `[THIRD-PARTY]` — [arXiv 2606.15828 Table IV](https://arxiv.org/html/2606.15828v2)

### F7 — Over-specification: positive directives and letter-over-intent following
- Guardrails Beat Guidance (679 rule files, 25,532 rules, 5,000+ agent runs): *"every individually beneficial rule is a negative constraint ('do not refactor unrelated code'), while every individually harmful one is a positive directive ('follow code style')."* The principle they draw: **"constrain what agents must not do, rather than prescribing what they should."** `[THIRD-PARTY]` — [arXiv 2604.11088](https://arxiv.org/abs/2604.11088)
- Vague prose and unverifiable directives are simply ignored: *"We value clean, well-tested code… please ensure all changes are properly tested"* produces no behaviour change; *"'Careful' isn't a constraint. 'Where possible' isn't a trigger condition. 'Gracefully' isn't a behavior specification."* `[THIRD-PARTY]` — [blakecrosley.com](https://blakecrosley.com/blog/agents-md-patterns)
- Nuance fragility: IFEval++ found instruction-following can drop **up to 61.8%** under subtle prompt rephrasing across 46 models — a brittle rule written one way may silently stop working when the agent paraphrases it. `[THIRD-PARTY]` — [arXiv 2512.14754](https://arxiv.org/abs/2512.14754)

### F8 — "Write a big overview" is the documented anti-pattern
- ETH Zurich: *"providing context files does not generally improve task success rates, while increasing inference cost by over 20% on average… while instructions in the context files are well followed by coding agents, **repository overviews, although popular and recommended by model providers, are not helpful**."* `[THIRD-PARTY]` — [arXiv 2602.11988](https://arxiv.org/abs/2602.11988)
- Two-agent ablation: correctness effect bounded to **<=10pp (Claude) / <=15pp (Codex)**, and *"a manipulation probe confirms the real AGENTS.md never converts a near-miss to a pass on either agent."* Reason: *"agents fail on implementation skill — feature design, pattern selection, exact wiring — not missing repository knowledge that a context file could supply."* `[THIRD-PARTY]` — [arXiv 2607.27250](https://arxiv.org/html/2607.27250v1)
- The generated-file trap: *"Never use initialization scripts to auto-generate your AGENTS.md. They flood the file with things that are 'useful for most scenarios' but would be better progressively disclosed. Generated files prioritize comprehensiveness over restraint."* `[THIRD-PARTY]` — [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md)

### F9 — Instructions are not enforcement
*"Claude treats CLAUDE.md files as context, not enforced configuration. To block an action regardless of what Claude decides, use a PreToolUse hook instead."* Anything that must never happen belongs in a hook/permission rule, not in the generated file. `[VENDOR]` — [Claude Code memory docs](https://docs.anthropic.com/en/docs/claude-code/memory)

---

## Recommended design for mcode

The repo already fixes the number. **`docs/01-north-star.md` contains no AGENTS.md/instruction-chain row** — its budget table covers cold start (<=15 ms), idle RSS (<=30 MB), binary size (<=25 MB), direct deps (<=12), *always-loaded tool schema* (<=3K tokens; core <=2.5K, <=3.5K with default extensions), harness-added TTFT (<=5 ms), and extension load cost (<=1 ms first, <=250 µs each after). `docs/32-second-batch.md` records this exact inconsistency: *"`AGENTS.md` says the budgets are 'defined in `docs/01-north-star.md'… but `docs/01` contains no session-start row… So one of the two is wrong."* `[INFERENCE]` from repo docs.

The authority for the instruction chain is **`docs/05-context-engineering.md` §Session start budget (line 71)**, cited by `docs/08`:

| Component | Budget |
|---|---|
| system prompt | <= 1.5K tokens |
| tools (default effective set) | <= 3.5K tokens |
| **instruction chain (AGENTS.md merge)** | **<= 2K tokens** |
| skill index | <= 1.5K tokens |
| **session-start total** | **<= 8.5K tokens** |

So **the generated AGENTS.md must respect a <=2K-token budget for the entire instruction chain**, not per file. Because org + user + root + ancestor files all share that 2K, a single file the generator writes should leave headroom: **target <=1.5K tokens for the file itself (~100-150 lines of Markdown, roughly 450-500 words)**. This is consistent with Anthropic's <200-line target, Codex's own 200-400-word `/init` target, HumanLayer's <60-line root, and Blake Crosley's <150-line rule of thumb. `[INFERENCE]` grounded in the table above plus the cited guidance.

Byte caps are **safety limits, not targets** (`docs/05` line 71: *"`08`'s byte caps are overflow safety limits, not targets"*):
- **32 KiB hard stop per file** (Codex's `project_doc_max_bytes` default).
- **64 KiB hard stop per chain** — hardcoded, deliberately not configurable (`docs/22-config-and-cli.md`).
- **8 KiB cap** for nested/subtree AGENTS.md injected JIT on first file access.
- Truncation cuts from the **broadest end** and **stops before the last entry**, so the closest file is never truncated; the cut leaves a visible `[truncated: over the instruction-chain budget; read <path> for the rest]` pointer rather than silent loss.
- Conflict resolution is **closest-wins**; user chat overrides everything; stated verbatim in mcode's system prompt.
- **No import/expansion syntax in v1** — `@path` imports are a budget hazard.

Additional repo constraints that bind `/init`:
- `docs/13-cli-and-tui.md`: `/init` generates the file rather than writing a scaffold — it submits a prompt and the agent explores the workspace and writes it — and it **refuses when an `AGENTS.md` already exists**. The file is the user's, and overwriting it would destroy the instructions the agent is meant to follow. Regeneration is deliberate: delete it first.
- `docs/09-memory.md`: *"Write it short (<=200 lines); put multi-step procedures in skills instead"* and *"Treating AGENTS.md as a correctness lever"* is listed as a thing that doesn't work.
- `docs/21-system-prompts.md` evidence table: *"AGENTS.md files do not improve task success and cost >20% more inference -> Budget the instruction chain hard; prune what the model can infer"*; long positive directives ("be thorough", "handle edge cases") are *"among the distorting rules whose removal **improves** pass rate"*.

**Net:** the generator's hard limit is 2K tokens for the whole chain; its *design* target for a single generated root file is ~1.5K tokens / <=150 lines; its output must contain **no** style rules a linter enforces, **no** file-by-file inventory, and **no** unverifiable prose.

---

### The generated section list

Ordered for a generated root `AGENTS.md`. Each section either carries the efficiency payoff (commands, verification) or is a measured anti-smell (negative constraints, pitched pointers). Sections that fail both tests are omitted deliberately.

| # | Section | Why it earns its place | Evidence |
|---|---|---|---|
| 1 | **`# AGENTS.md` + one-sentence project description** | Anchors every decision; "acts like a role-based prompt". Must be one sentence, not an overview essay — repository overviews are the *one* content class measured as unhelpful. | [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md); [arXiv 2602.11988](https://arxiv.org/abs/2602.11988) |
| 2 | **Build / test / lint / typecheck commands** — exact invocations *with flags*, in one early block | The single most-cited success factor: *"Put relevant executable commands in an early section… Include flags and options, not just tool names."* Commands are how the agent gets anything done. | [GitHub blog](https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/); [blakecrosley.com](https://blakecrosley.com/blog/agents-md-patterns); [apache/airflow](https://raw.githubusercontent.com/apache/airflow/main/AGENTS.md) |
| 3 | **Definition of done / verification** — the exact exit codes or commands that prove completion | *"The agent reports 'done' without verifying"* is the most common failure mode; closure definitions eliminate it. Anthropic: give the agent "a check it can run" or you become the verification loop. | [blakecrosley.com](https://blakecrosley.com/blog/agents-md-patterns); [Claude Code best practices](https://www.anthropic.com/engineering/claude-code-best-practices) |
| 4 | **Non-obvious constraints & gotchas** — phrased as **negative constraints** ("do not refactor unrelated code", "never commit secrets") | Negative constraints are the only rule class measured as individually beneficial; positive directives ("follow code style") are individually harmful. "Never commit secrets" was the most common helpful constraint in 2,500+ repos. | [arXiv 2604.11088](https://arxiv.org/abs/2604.11088); [GitHub blog](https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/); [Claude Code best practices](https://www.anthropic.com/engineering/claude-code-best-practices) |
| 5 | **Boundaries: Always / Ask first / Never** | Explicit three-tier boundaries prevent destructive mistakes; *"the 'Never' list is as important as the escalation paths."* | [GitHub blog](https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/); [blakecrosley.com](https://blakecrosley.com/blog/agents-md-patterns) |
| 6 | **When blocked / escalation rules** | Without them, agents "default to increasingly creative workarounds when blocked — deleting lock files, bypassing checks, or silently ignoring failures." | [blakecrosley.com](https://blakecrosley.com/blog/agents-md-patterns) |
| 7 | **Repository structure as *capabilities*, not a file inventory** — name the 3-6 subsystems and what lives where, at directory granularity | HumanLayer's WHAT/WHY/HOW requires a map, especially in monorepos — but *"document file system structure"* is the highest-staleness content and a documented poisoning mechanism. Directory-level capability descriptions survive refactors; per-file paths do not. | [HumanLayer](https://www.humanlayer.dev/blog/writing-a-good-claude-md); [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md); [arXiv 2606.15828 §VI-A](https://arxiv.org/html/2606.15828v2) |
| 8 | **Non-standard conventions the agent cannot infer** — package manager, non-default build/test workflow, house patterns that differ from language defaults | The only content class with measured correctness value: context files "are useful for specifying non-standard coding practices". Anthropic's include column is exactly this. | [arXiv 2602.11988](https://arxiv.org/abs/2602.11988); [Claude Code best practices](https://www.anthropic.com/engineering/claude-code-best-practices) |
| 9 | **Pitched pointers to deeper docs** — `path` + what it contains + when to read it | Progressive disclosure keeps the always-on file inside budget; the *pitch* is required, because a bare path is a Blind Reference the agent ignores or over-loads. | [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md); [HumanLayer](https://www.humanlayer.dev/blog/writing-a-good-claude-md); [arXiv 2606.15828 §IV-D](https://arxiv.org/html/2606.15828v2) |
| 10 | **Commit / PR / CI conventions** (only if they are non-obvious or enforced) | A "popular choice" section and part of Codex's own recommended outline; genuine repo etiquette the agent can't infer. Keep to conventions that are actually enforced or non-default. | [agents.md](https://agents.md/); [Codex init prompt](https://raw.githubusercontent.com/kekmodel/codex-system-prompts/main/prompts/tui/tui-init-command.md) |

**Explicitly excluded** (each is a named smell or measured anti-pattern): code-style/formatting rules a linter enforces (Lint Leakage, 62%); rare multi-step procedures (Skill Leakage, 35% — route to a skill); file-by-file descriptions; standard language conventions; API documentation (link instead); anything that changes frequently; unverifiable prose and ambiguous directives; long positive directives ("be thorough"); anything derivable from reading the code. `[VENDOR]`+`[THIRD-PARTY]` — [Anthropic include/exclude](https://www.anthropic.com/engineering/claude-code-best-practices), [arXiv 2606.15828](https://arxiv.org/html/2606.15828v2), [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md), [arXiv 2604.11088](https://arxiv.org/abs/2604.11088)

**Generator prompt guidance that follows from the evidence.** (a) Explore, then write; do not paste the exploration into the file. (b) For every candidate line ask *"Would removing this cause the agent to make a mistake?"* — if not, cut. (c) Prefer negative constraints over positive directives. (d) Emit exact commands, not descriptions of commands. (e) Omit any section it cannot fill with something non-obvious and stable. (f) Stop at the budget: <=1.5K tokens / <=150 lines for the file. (g) On an existing AGENTS.md, propose a diff — never overwrite. `[INFERENCE]` from [Claude Code best practices](https://www.anthropic.com/engineering/claude-code-best-practices), [arXiv 2604.11088](https://arxiv.org/abs/2604.11088), [GitHub blog](https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/), [AI Hero](https://www.aihero.dev/a-complete-guide-to-agents-md), [docs/13](docs/13-cli-and-tui.md)

---

## Traps

Each of these is a way `/init` produces a file that looks right and works worse than nothing.

1. **Writing the file before exploring.** The failure mode is a plausible file: a
   `pytest` command that is not what CI runs, a "src/ contains the source" line.
   Every claim in the generated file must trace to a file the agent read.
2. **Restating the linter.** Indentation, line length, naming, import order and
   docstring rules are the most common content in real files (Lint Leakage, 62%)
   and the least useful: a deterministic tool already enforces them, and every
   line spent on them is a line not spent on a trap. The prompt names this
   explicitly because it is the default behaviour otherwise.
3. **A repository overview.** The one content class measured as unhelpful, at
   over 20% inference cost. One sentence, not a section.
4. **Positive directives.** "Always write tests", "be careful with migrations" —
   the rule class measured as individually *harmful*. Phrase as "do not X".
5. **Padding a small repository.** A small project has few traps, and a file that
   invents sections to fill is worse than a short one. The prompt says so.
6. **Fossilization.** 24 of 100 sampled files had never been edited after
   generation. The prompt asks the model to report its least-certain lines, so
   the file gets reviewed rather than trusted.
7. **Treating the file as enforcement.** It is context, not configuration.
   Anything that must *never* happen belongs in a permission rule or a hook
   (`docs/12`), not in prose the model can decide to ignore.

## Open questions

- Does a generated file measurably beat the scaffold it replaced? The research
  gives a target shape and a size budget, not a result. The eval suite is where
  that would be settled, and it has no AGENTS.md case yet.
- The 2K-token instruction-chain budget is shared by every file in the chain, so
  a generated root file can crowd out a user's own. Should `/init` refuse when
  the chain is already near budget, or trim to fit? It currently neither measures
  nor trims.
- `docs/01-north-star.md` has no instruction-chain row, though `docs/32` records
  that it is supposed to own that budget. The number lives in `docs/05` instead.

## Sources

Every URL below was actually retrieved for this report.

**Standard & vendor docs**
- https://agents.md/ — AGENTS.md definition, 60k+ projects, adoption list, no-required-fields FAQ, closest-wins conflict rule, 88-file monorepo nesting, AAIF/Linux Foundation stewardship, migration snippets.
- https://learn.chatgpt.com/docs/agent-configuration/agents-md — Codex discovery precedence (global -> root -> cwd), `AGENTS.override.md`, `project_doc_max_bytes` = 32 KiB default, merge order, code-review rules section.
- https://docs.anthropic.com/en/docs/claude-code/memory — CLAUDE.md load order, **<200-line target**, "longer files reduce adherence", contradiction -> arbitrary choice, `/doctor prompt-audit`, `/init` behavior, AGENTS.md read directly, hooks vs context.
- https://www.anthropic.com/engineering/claude-code-best-practices — "bloated CLAUDE.md files cause Claude to ignore your actual instructions", the include/exclude table, "give Claude a way to verify its work", `/init` guidance, hooks for enforcement.
- https://raw.githubusercontent.com/kekmodel/codex-system-prompts/main/prompts/tui/tui-init-command.md — Codex `/init` prompt verbatim: title "Repository Guidelines", **200-400 words optimal**, recommended section outline. (The upstream `openai/codex` raw path returned HTTP 404; this mirror carries the file with source path, commit and token count.)
- https://raw.githubusercontent.com/apache/airflow/main/AGENTS.md — a real top-tier AGENTS.md: ~200 lines, exact command block, negative constraints, Always/Ask-first/Never boundaries, References section.

**Guidance & analysis**
- https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/ — analysis of 2,500+ repos: commands early, examples over explanations, three-tier boundaries, six core areas, "most agent files fail because they're too vague".
- https://www.aihero.dev/a-complete-guide-to-agents-md — instruction budget, staleness poisoning, "never auto-generate", progressive disclosure, monorepo nesting table.
- https://www.humanlayer.dev/blog/writing-a-good-claude-md — 150-200 instruction ceiling, uniform degradation, primacy/recency bias, ~50 instructions already in [CC]'s system prompt, <60-line root file, "LLM is not a linter", don't `/init`.
- https://blakecrosley.com/blog/agents-md-patterns — what gets ignored (prose, ambiguous directives, contradictory priorities, unenforced style), command-first, closure definitions, escalation rules, section <50 lines / file <150 lines, "acid test".
- https://developer.upsun.com/posts/ai/agents-md-less-is-more — secondary summary of the "too long" research; "start empty, build incrementally".

**Research papers (primary)**
- https://arxiv.org/abs/2507.11538 — IFScale: 500 instructions, best frontier model 68%, three degradation patterns, bias toward earlier instructions.
- https://arxiv.org/abs/2602.11988 — Gloaguen et al.: context files don't generally improve success, **+>20% inference cost**, instructions well followed but repository overviews unhelpful.
- https://arxiv.org/html/2607.27250v1 — two-agent ablation, 288 runs: correctness null bounded <=10/15pp, failures are implementation skill not missing knowledge, agent-specific borderline tasks, blind-full-suite-run reduction.
- https://arxiv.org/html/2601.20404v2 — Lulla et al., 124 PRs: median runtime -28.64%, median output tokens -16.58%, mean wall-clock -20.27%.
- https://arxiv.org/html/2606.15828v2 — Configuration Smells in AGENTS.md: six smells, prevalence (Lint Leakage 62%, Context Bloat 42%, Skill Leakage 35%, Init Fossilization 24%, Conflicting Instructions 28%/57% precision, Blind Reference 16%), 91/100 files affected, co-occurrence lift.
- https://arxiv.org/abs/2604.11088 — Guardrails Beat Guidance: 679 rule files / 25,532 rules / 5,000+ runs; negative constraints beneficial, positive directives harmful, both +13.8pp; gains content-independent.
- https://arxiv.org/abs/2512.14754 — IFEval++: instruction-following can drop up to 61.8% under nuanced prompt rephrasing (46 models).
- https://arxiv.org/abs/2307.03172 — Lost in the Middle: position-dependent degradation, best at beginning/end, worst in the middle.
- https://www.trychroma.com/research/context-rot — 18 LLMs, performance degrades with input length on simple tasks; distractors amplify; haystack structure matters.

**Repo docs (read-only)**
- `docs/01-north-star.md` — hard budgets (cold start <=15 ms, RSS <=30 MB, binary <=25 MB, deps <=12, always-loaded tool schema <=3K tokens, TTFT <=5 ms, extension load cost); no instruction-chain row.
- `docs/05-context-engineering.md` — §Session start budget (line 71): system prompt <=1.5K + tools <=3.5K + **instruction chain <=2K** + skill index <=1.5K = <=8.5K tokens; "`08`'s byte caps are overflow safety limits, not targets".
- `docs/08-skills-and-agents-md.md` — AGENTS.md discovery/merge, closest-wins, **<=2K tokens for the whole chain**, 32 KiB/file and 64 KiB/chain hard stops, 8 KiB nested JIT cap, no imports in v1.
- `docs/09-memory.md` — "Write it short (<=200 lines); put multi-step procedures in skills"; "Treating AGENTS.md as a correctness lever" is listed under what doesn't work.
- `docs/13-cli-and-tui.md` — `/init` writes AGENTS.md **only when none exists**; never overwrite the user's file.
- `docs/21-system-prompts.md` — evidence table: AGENTS.md no success gain + >20% cost; long positive directives are distorting rules whose removal improves pass rate.
- `docs/22-config-and-cli.md` — `instruction_chain_bytes = 32768` per file; 64 KiB chain total hardcoded, not configurable.
- `docs/32-second-batch.md` — records that `docs/01` is missing the session-start budget row it is supposed to own.
