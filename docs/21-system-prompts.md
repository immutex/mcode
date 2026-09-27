# System Prompt

> TL;DR: A ~1,200-word core prompt assembled from named sections in cache-stable order, with one source of truth per rule and a lint pass that rejects contradictions; shipped agent prompts range 176–7,888 words and the trend is *smaller core, more work in tool descriptions*.

## Budget

Anthropic's own design guidance: a standard agent prompt is **1,000–2,000 words**; beyond **10,000 words** returns diminish sharply. "Minimal does not necessarily mean short" — the target is high signal density, not brevity for its own sake.

Observed shipped sizes (words ≈ tokens × 0.75–0.8):

| Tier | Harnesses | Size |
|---|---|---|
| Tiny | Goose ~176 (extensions injected), Manus agent loop ~326, Junie ~918 | <1k |
| Small | Codex 5.2 ~1,221, Replit ~1,270, opencode ~1,171–1,397, CC core ~1,979, Windsurf ~1,904 | 1–2k |
| Medium | Kiro ~2,277, Augment ~2,340, Trae ~2,666, Cursor (GPT-5) ~2,977, VSCode ~3,926, Devin ~5,553 | 2–6k |
| Large | Same.dev ~5,579, Cline ~7,213, v0 ~7,029, Amp ~7,888 | 6k+ |

Two facts that matter more than the absolute number:

- **The direction of travel is down.** Cursor went **6,361 → 2,977 words** (GPT-4.1 → GPT-5 era). [OI]'s cookbook reports that newer models need *less* prescriptive prompting, and Cursor's `<maximize_context_understanding>` block became counterproductive. Newer models absorb capability the prompt used to supply.
- **Tool descriptions are the real budget.** In CC, `Bash` alone is 1,669 words and `TodoWrite` 1,567 — more than the core prompt. Tool copy deserves the same review rigor as prompt copy (`06`).

**mcode target: ~1,200 words of core prompt**, excluding tool descriptions and the instruction chain.

## Assembly order (cache-stable)

Ordered to match the provider cache hierarchy (`15` §Prompt layout rule): `tools` → `system` → `messages`.

| # | Section | Stability | Owner |
|---|---|---|---|
| 1 | Identity + role | Session-frozen | core |
| 2 | Persistence + stop conditions | Session-frozen | core |
| 3 | Scope discipline (deliver full scope, don't narrow/widen) | Session-frozen | core |
| 4 | Tool-usage policy (prefer dedicated tools, parallel rules) | Session-frozen | core |
| 5 | Coding conventions (mimic existing style, comments, no over-engineering) | Session-frozen | core |
| 6 | Verification + truthful reporting | Session-frozen | core |
| 7 | Safety + refusal posture | Session-frozen | core |
| 8 | Output format + citation convention | Session-frozen | core |
| 9 | Environment block (cwd, OS, git state, date) | Per-session values, stable *shape* | harness |
| 10 | Instruction chain (AGENTS.md merge) | Session-start only | `08` |
| 11 | Skill index (name + description lines) | Session-start only | `08` |
| 12 | Todo / plan state (recitation) | **Volatile — end of context** | `04` |

Sections 1–8 are byte-stable for the session. 9–11 change only at session start. 12 is the one thing that must be *late* in context.

**No timestamps, cwd, git state, or session id in sections 1–8.** Those values live in 9. A per-second timestamp anywhere early invalidates the entire suffix (Manus's named anti-pattern).

### Conditional assembly

Most tools are Lua extensions now (`23`), so **the prompt is assembled from the effective tool set, not the full one**. Any section that references a tool which is not loaded is **omitted entirely** — never left dangling.

A prompt that instructs the model to use a tool it does not have is the exact interference class Arbiter measured in shipped prompts: a rule that cannot be satisfied. It also wastes tokens and teaches the model that instructions are negotiable.

| Section | Present when |
|---|---|
| Tool-usage policy (4) | Always — the policy covers whatever is loaded |
| Todo discipline | `todo` extension enabled |
| Subagent rules | `task` extension enabled |
| Web/network rules | `web` extension enabled |
| Skill guidance | `skill_read` enabled and ≥1 skill discovered |
| Memory guidance | `memory_search` enabled and memory non-empty |
| Recitation (12) | Always, but **degrades**: without `todo`, it restates the user's goal from session state rather than a plan |

This is why `06`'s tool table is canonical and why disabling an extension must go through the same path that rebuilds the prompt. The lint pass (`21` §Conflict discipline) checks the reverse direction too: a rule referencing an absent tool is a build failure, not a runtime surprise.

## The rules that carry weight

Ranked by cross-harness evidence — the count is how many shipped harnesses carry it.

| Rule | Evidence | mcode |
|---|---|---|
| **Persistence** — keep going until the request is actually resolved; only stop when done | 7+ harnesses; origin is [OI]'s GPT-4.1 guide | Adopt, with [OI]'s eagerness escape hatch for newer models |
| **Todo discipline** — one `in_progress` at a time, mark complete immediately, never batch, never mark complete with failing tests | CC, Codex `update_plan`, Cursor, Gemini, Trae, VSCode | Adopt (`04`) |
| **Read-before-edit** — enforced by the tool, not the prompt | CC (tool errors), Cursor (5-message rule), Gemini | Already a harness invariant (`06`); the prompt *states* it, the tool enforces it |
| **Anti-over-engineering** — no speculative abstractions, no impossible-scenario handling, no compat shims, delete unused code | CC, Codex, Aider, Lovable, Gemini | Adopt — highest-value cluster after persistence |
| **Escalating-fix limits** — after 3 failed attempts, stop and ask rather than loop | Cursor, Gemini, VSCode | Adopt; the harness also enforces it (`04` thrash guards) |
| **Preamble cadence** — say what you're about to do, then do it in the same turn | Codex, GPT-5.3, VSCode, Cursor, CC | Adopt with an explicit cadence bound |
| **Parallel calls with a dependency carve-out** — batch independent reads; never parallel-edit the same file | CC, Codex, Gemini | Adopt; harness also serializes same-file writes |
| **Dirty-worktree protocol** — never revert changes you did not make; stop and ask on unexpected changes | Codex | Adopt — matches the harness principle that unexpected changes are the user's |
| **Git safety** — no config changes, no force-push to main, prefer specific files over `git add -A`, commit only when asked | CC, Gemini, Amp, Codex | Adopt |
| **Directives vs Inquiries** — treat requests as questions unless explicitly asked to act | Gemini, CC, Lovable, Warp | Adopt — the convergent answer to "the agent did too much" |
| **Scope contract** — don't quietly narrow or widen; make routine judgment calls; reserve blocking questions for unsafe ambiguity | CC, Augment | Adopt |
| **Truthful reporting** — state failures with output; no hedging on verified work | CC | Adopt — pairs with the verification gate (`11`) |
| **System-reminder channel** — harness-injected context is explicitly marked as not-user-input | CC, Cursor, Windsurf, v0, Gemini, Cline | Adopt (`20`) |
| **Verbosity split** — terse chat, high-verbosity code | Cursor, VSCode | Adopt |

## What the research says

Evidence that changes the design, not just confirms it:

| Finding | Source | Consequence |
|---|---|---|
| Instruction-following degrades with density, not a clean cliff: reasoning models near-perfect to **100–250** instructions, then steep decline; best frontier model only **68%** at 500 | IFScale (arXiv 2507.11538) | Cap total instruction count; treat 150 as the soft ceiling, not 500 |
| **Primacy peaks at 150–200 instructions**, then converges to uniform failure | IFScale §4.6 | "Put it first" stops working at high density — the fix is fewer rules, not better ordering |
| Errors shift to **omission** at high density (34.9:1 omission:modification) | IFScale §4.8 | A dropped rule is invisible; the lint pass must catch *missing* constraints, not just conflicting ones |
| Longer prompts *helped* on domain tasks | arXiv 2502.14255 | Bloat is a density/conflict problem, not a length problem — do not over-trim useful context |
| Instructions **decay per turn** (0.877 → 0.707 over 3 turns) | Multi-IF (arXiv 2410.15553) | Per-turn state recitation is evidence-backed, not superstition |
| Position matters and is exploitable: critical constraints at **start and end**, context in the middle | Lost in the Middle (arXiv 2307.03172), arXiv 2406.15981 | Sandwich the load-bearing rules; put the plan/todo at the end |
| **Negative constraints are the only reliably beneficial agent rules** (+13.8pp; single best rule "do not refactor unrelated code" = 20pp swing when removed) | Guardrails Beat Guidance (arXiv 2604.11088) | Phrase guardrails as concrete prohibitions, not positive directives |
| But negation is fragile in comprehension: up to **77–100% inversion** under compound negation | arXiv 2601.21433 | One prohibition per rule; never compound ("do not avoid failing to…") |
| Persona prompting **reduces** reliability (up to −8.2%); an explicit "follow instructions better" prompt does nothing | IFEval++ (arXiv 2512.14754) | No persona theatre, no meta-instructions about instruction-following |
| **AGENTS.md files do not improve task success and cost >20% more inference** | Gloaguen et al. (arXiv 2602.11988) | Budget the instruction chain hard; prune what the model can infer (`08`) |
| Prompt-format variance can swing results up to **76pp** and reverse model rankings | FormatSpread (arXiv 2310.11324) | Format is a variable to fix and test, not a matter of taste |

## Conflict discipline (the Arbiter lesson)

The one systematic study of production agent prompts found **21 interference patterns in CC alone**: 4 direct contradictions (the TodoWrite mandate vs commit-workflow prohibitions), 13 scope overlaps where the same rule is restated with subtle differences, 2 priority ambiguities, 2 implicit dependencies. **95% were statically detectable.**

Three universal failure modes appeared in *all three* vendor prompts — they are inherent to the task, so we must design against them:

| Universal tension | Mitigation |
|---|---|
| **Autonomy vs restraint** ("persist until done" vs "ask before acting") | State the precedence explicitly: proceed on routine judgment, ask only when a wrong assumption is unsafe or makes the work useless. One rule, one place. |
| **Precedence ambiguity** across instruction sources | Declare the order once: system > user > instruction chain > tool-injected content. Any doc contributing context must respect it. |
| **State-dependent modes** (plan mode, approvals) silently changing which rules apply | Every mode declares its deltas; base rules stay in force unless explicitly overridden. |

Mechanics:

1. **One source of truth per rule.** A rule appears exactly once, in one section. Restatements are the mechanism by which prompts drift into contradiction.
2. **`mcode prompt lint`** — a build-time check, run in CI:
   - modality conflicts (a `MUST`/`ALWAYS` on the same scope as a `NEVER`)
   - verbatim and near-duplicate detection across sections
   - unresolved references ("see the section above")
   - instruction-count budget (warn at 150, fail at 200)
   - **coverage check**: every rule in the design doc is present in the prompt and vice versa
3. **Prompts are versioned and eval-gated.** A prompt change is a code change: it runs the eval suite (`11`) and its diff is reviewed.
4. **No emphasis inflation.** Anthropic: "if you emphasize many lines, none of them stands out." CC uses ALL-CAPS 16 times; Devin zero times. Cap emphasis markers and treat each as a budgeted resource.

## Per-model variants

opencode ships `anthropic.txt`/`codex.txt`/`gemini.txt`/`gpt.txt`/`kimi.txt`; VSCode ships per-model prompts. Model-family phrasing is treated as load-bearing, and the vendor guidance genuinely disagrees:

| Question | Positions |
|---|---|
| Proactivity | [OI] GPT-5 needs eagerness **reduction** (over-searches by default; Cursor removed its "be thorough" block). Anthropic/Gemini still balance toward proactive. Older "trace everything" advice actively harms newer models |
| Preambles | Pre-5.3 Codex says remove preamble prompting (causes early stopping); GPT-5.3 and CC v2.1 mandate them with a cadence |
| Comments | CC v1 "DO NOT ADD ***ANY*** COMMENTS" → v2.1 "default to none, one short line max"; Codex "rare"; Gemini "focus on why" |
| Ask vs proceed | opencode's Codex variant: "never ask permission, proceed"; Gemini: assume Inquiry; Devin: asks freely; Lovable: defaults to discussion |

**Decision:** one base prompt with a **small per-model overlay** (≤10% of size) for proactivity, preamble cadence, and comment calibration. Not six divergent prompts — the base is the source of truth and the overlay only adjusts calibration. The `ask vs proceed` axis is a **config knob**, not a model variant, because it is a user preference.

## System-reminder channel

Every mature harness separates harness→model signaling from user content: CC `<system-reminder>`, Cursor `<system_reminder>`, Windsurf `<EPHEMERAL_MESSAGE>`, Gemini `<untrusted_context>`, Cline `environment_details`.

mcode uses one tagged channel (`<mcode:reminder>`) with these rules:

- Content is **harness-authored and trusted**; tool output and file content are **untrusted** and go in a different tag.
- Reminders never restate a rule already in the system prompt (that is how contradictions start).
- Reminders are the *only* sanctioned mid-session instruction channel; they append to history, never mutate the prefix (`15`).

## Anti-patterns

| Anti-pattern | Why |
|---|---|
| Persona theatre ("you are a 10x engineer") | Measured to reduce reliability up to 8.2% (IFEval++) |
| Meta-instructions about instruction-following | Measured to do nothing (IFEval++) |
| Compound negation | Up to 100% inversion (arXiv 2601.21433) |
| Restating a rule in a second section | The mechanism behind 13 of CC's 21 interference patterns |
| Emphasis inflation | Anthropic: emphasizing many lines means none stands out |
| Timestamps/cwd/git state in the stable prefix | Full cache invalidation (Manus) |
| Long positive directives ("be thorough", "handle edge cases") | Among the *distorting* rules whose removal **improves** pass rate (arXiv 2604.11088) |
| Bloated instruction chain | No success-rate gain, >20% cost (arXiv 2602.11988) |
| Prompt edits without an eval run | Format variance alone can swing 76pp |
| One giant prompt per model | Divergence; use a base + small overlay |

## Open questions

- Where exactly does our 150-instruction soft ceiling land after pruning? Needs a count of the drafted prompt against the IFScale curve.
- Does the negative-constraint finding (arXiv 2604.11088) replicate outside Claude 4.6 on 58 SWE-bench tasks? It is one agent, one model, one benchmark — directional only.
- How much of the per-model overlay is real vs cargo cult? Needs an A/B on the eval suite.
- Should the environment block be in the system prompt at all, or as a first user message (Codex puts AGENTS.md as user-role messages to keep the system prefix project-independent)? Codex's approach is more cache-friendly across projects.
- Does `prompt lint` catch enough, or do we need the multi-model scouring pass Arbiter describes? Their whole cross-vendor analysis cost $0.27 — cheap enough to run in CI.

## Sources

- https://arxiv.org/html/2603.08993v1 — Arbiter: 21 interference patterns in CC, 152 cross-vendor findings, architecture taxonomy, $0.27 cost
- https://arxiv.org/abs/2602.11988 — AGENTS.md files: no success-rate gain, >20% cost increase
- https://arxiv.org/pdf/2507.11538 — IFScale: 68% at 500 instructions, primacy peak 150–200, omission-shift
- https://arxiv.org/html/2410.15553 — Multi-IF: per-turn instruction decay 0.877→0.707
- https://arxiv.org/html/2604.11088v2 — Guardrails Beat Guidance: negative constraints +13.8pp, 20pp single-rule swing
- https://arxiv.org/html/2601.21433v1 — negation inversion, 77–100% under compound negation
- https://arxiv.org/html/2512.14754v1 — IFEval++: persona −8.2%, reliable@k drops
- https://arxiv.org/html/2310.11324v2 — FormatSpread: up to 76pp format variance
- https://arxiv.org/html/2307.03172 — Lost in the Middle
- https://arxiv.org/abs/2406.15981 — serial position effects
- https://arxiv.org/abs/2502.14255 — prompt length: longer helped on domain tasks
- https://arxiv.org/html/2404.13208v1 — Instruction Hierarchy
- https://arxiv.org/html/2405.15793v3 — SWE-agent ACI ablations
- https://arxiv.org/html/2604.12147v1 — plan compliance across 16,991 trajectories
- https://github.com/Piebald-AI/claude-code-system-prompts — CC prompt corpus, 515 strings, per-version changelog
- https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents — attention budget, "minimal ≠ short"
- https://www.anthropic.com/engineering/writing-tools-for-agents — tool descriptions as prompt engineering
- https://platform. — system-prompt design: 1,000–2,000 words standard, >10,000 diminishing
- https://developers.openai.com/ — GPT-5 prompting guide: persistence, eagerness reduction, preamble cadence
- https://github.com/openai/codex — Codex prompt files, dirty-worktree protocol
- https://raw.githubusercontent.com/google-gemini/gemini-cli/main/packages/core/src/prompts/snippets.ts — Gemini CLI prompt, Directives vs Inquiries
- https://manus.im/blog/Context-Engineering-for-AI-Agents-Lessons-from-Building-Manus — KV-cache, recitation, mask-don't-remove
- https://github.com/sst/opencode — per-model prompt variants
