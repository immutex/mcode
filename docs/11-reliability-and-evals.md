# Reliability and Evals

> TL;DR: Reliability comes from grounding every claim in a tool result and grading every run with deterministic verifiers; evals are a cheap fixture-suite plus pass^k consistency tracking, with explicit anti-reward-hacking checks.

## State of the field

### Benchmarks (numbers as reported, with dates)

| Benchmark | What it measures | Reported SOTA / reference | Date | Source |
|---|---|---|---|---|
| SWE-bench Verified (500 Python issues) | real repo bug fixing | 79.2% resolved (Claude Opus 4.5, live-SWE-agent / Sonar Foundation Agent); Anthropic self-reported 80.9% high-effort | Dec 2025 | swebench.com; Opus 4.5 system card |
| SWE-bench Multilingual (300 tasks, 9 langs incl. C/C++, Rust) | cross-language repair | 76.2% (Claude Opus 4.5, self-reported); baseline Claude 3.7 Sonnet/SWE-agent was ~43% | Nov 2025 / Feb 2025 | swebench.com/multilingual.html; system card |
| SWE-bench Pro (Scale AI, 1,865 tasks, private split) | harder, longer-horizon repair | GPT-5 23.3% public; Claude Opus 4.1 17.8% commercial split | Sep 2025 | scale.com/blog/swe-bench-pro; arXiv:2509.16941 |
| Terminal-Bench 1.0 | real terminal tasks (build, debug, sysadmin) | 43.3% (Claude Opus 4.1) | 2025 | paperswithcode Terminal-Bench 1.0 |
| Terminal-Bench 2.0 | harder terminal tasks | 59.27% (Claude Opus 4.5, 128K thinking; self-reported) | Nov 2025 | Opus 4.5 system card |
| tau-bench (retail/airline, tool+policy+state) | multi-turn tool-agent reliability | Claude 3.5 Sonnet: retail pass^1 69.2% / pass^4 46.2%; airline 46.0% / 22.5%. GPT-4o retail pass^8 < 25% (paper) | Jun 2024, ICLR 2025 | arXiv:2406.12045; sierra-research/tau-bench |
| Aider polyglot (225 edits, 6 langs) | code-edit format + correctness | GPT-5 (high) 88.0% correct, 91.6% well-formed edits | Aug 2025 | aider.chat/docs/leaderboards |
| LiveCodeBench | contamination-free competitive coding | rolling window; see live leaderboard [UNVERIFIED numbers here — check at build time] | rolling | livecodebench.github.io |
| WebArena (812 web tasks) | realistic web agent tasks | 71.2% best scaffolded agent (Dec 2025); 65.3% best single-policy (Claude Opus 4.5, self-reported) | Dec 2025 | personalagentbench.com; system card |
| GAIA (165 public validation Qs) | general assistant reasoning+tools | 74.55% (HAL standardized, Claude Sonnet 4.5) | Sep 2025 | hal.cs.princeton.edu/gaia |

Key gap to internalize: tau-bench's pass^1→pass^4 collapse (69→46% retail, 46→23% airline) is the single best public illustration that *capability* and *reliability* are different axes. mcode's evals must measure both.

### Papers and evidence on failure modes

| Work | Finding | Source |
|---|---|---|
| Anthropic, "Sycophancy to Subterfuge" (arXiv:2406.10162, Jun 2024) | RL on gameable environments generalizes zero-shot from sycophancy to reward-tampering: 45 reward-tampering episodes, 7 with test-editing concealment, in 32,768 eval episodes; <1/1000 successful concealment. HHH training reduced but did not eliminate. | arxiv.org/abs/2406.10162; anthropic.com/research/reward-tampering |
| Anthropic, "Towards Understanding Sycophancy in LMs" (arXiv:2310.13548) | RLHF/RLAIF preference tuning systematically rewards agreeing with the user over correctness. | arxiv.org/abs/2310.13548 |
| METR, "Recent Frontier Models Are Reward Hacking" (Jun 2025) | o3 hacked 30.4% of RE-Bench runs (39/128) vs 0.7% of HCAST (8/1087) — >43x more when the scoring function is exposed/optimizable. Hacks: monkey-patching graders, falsifying timers, returning reference answers. | metr.org/blog/2025-06-05-recent-reward-hacking |
| METR, GPT-5 eval report (2025) | LLM monitor over 789 runs flagged 49; manual review confirmed 15 (+3 via keyword/audit). Rescoring 18 runs (~2%) as failures moved the 50%-task-horizon estimate from ~3h to ~2h15m. Human review of automated hack flags is mandatory. | metr.org/evaluations/gpt-5-report |
| METR, "Measuring AI Ability to Complete Long Software Tasks" (Mar 2025) | 50%-reliability horizon doubles ~every 7 months; measured on curated software/ML tasks, not general work. | metr.org/blog/2025-03-19-measuring-ai-ability-to-complete-long-tasks; arXiv:2503.14499 |
| OpenAI, "Why Language Models Hallucinate" (Sep 2025) | Binary grading makes guessing optimal; proposes confidence-threshold scoring: correct +1, "I don't know" 0, wrong −t/(1−t). Most benchmarks (incl. SWE-bench) select for bluffing. | openai.com/index/why-language-models-hallucinate; arXiv:2509.04664 |
| Zheng et al., MT-Bench judge study (NeurIPS 2023) | LLM-judge pathologies: position bias (GPT-4 consistent after swap only 65%), verbosity attack failure rates 91.3% (Claude-v1, GPT-3.5) vs 8.7% (GPT-4), self-preference ~10–25pp. | arXiv:2306.05685 |
| OpenAI, "Separating signal from noise in coding evals" (2025) | Contamination and scaffold variance dominate small-n coding evals; use held-out, freshly-written tasks. | openai.com/index/separating-signal-from-noise-coding-evaluations |

## What works / what doesn't

**Works:**
- Deterministic verification (compile, tests, typecheck) as the sole "success" oracle. Every strong scaffold (SWE-agent, Claude Code, Aider) gates edits on a real test run. Cheap, unfakeable, reusable.
- Read-before-write invariants. Edit tools that require the model to have read the exact byte range being edited eliminate most phantom-file and stale-content failures mechanically — no prompting needed.
- Anchor-matched edits (Aider's approach: search/replace blocks with uniqueness checks; 91.6% well-formed edit rate on polyglot). Rejecting a fuzzy match and forcing re-read beats silently applying a wrong match.
- pass^k for reliability reporting; pass@k for capability. Report both or you flatter yourself.
- Fresh/rolling task sets (LiveCodeBench, SWE-bench Pro private split) to defeat contamination.
- LLM-as-judge only for open-ended outputs, never for pass/fail on code — and only with position-swap + tie handling.

**Doesn't work:**
- Prompt-only anti-hack rules ("don't modify tests"). METR's data: hacks concentrate where the grader is exposed. Structural separation of grader from agent beats instructions.
- Self-evaluation as ground truth. Self-consistency helps sampling, not verification; models agree with their own errors.
- Small-n benchmarks as regression gates: 30-task suites swing ±15pp run-to-run at these pass rates. Use paired/deterministic fixtures instead.
- Trusting benchmark scores without hack audits: METR moved GPT-5's headline number ~15% by manually reviewing 2% of runs.

## Recommended design for mcode

### The dominant failure mode: Coherence Collapse

The largest study of coding-agent failures to date (16,758 trajectories, 3 architectures, 7 models) found something that reframes what a harness is *for*:

- **Failures are patch-quality-bound, not localization-bound.** 60–69% of failures on capable models *reach and edit the correct functions* and still fail. Parallel exploration and repo indexing buy less than believed.
- **Coherence Collapse — reaching the right code, then overwriting or thrashing it — is 39.7% of edit-quality failures** (SWE-bench), 32.3% on PolyBench, 42.1% on JavaScript. "Confused Thrashing" (3+ edit attempts on the gold file, none persisting) is 29.1% of that.
- It is **not** long-context degradation — the sub-type is length-independent.
- **Edit-commit checkpointing is an existence proof, not a theory**: five runs produced an intermediate edit that was *bit-identical to the gold patch*, then destroyed it. Re-submitting that checkpointed edit passed the full test suite in all five cases.

**Consequence for mcode:** checkpointing is not a safety feature bolted on for user comfort — it is an **accuracy lever**. The per-edit inverse record (`03` §Undo and rollback) exists to recover from Coherence Collapse, and the verification gate should be able to re-submit an earlier passing intermediate state rather than trusting the final tree.

A weaker signal from the same work: cross-model **consensus as confirmation** during a run gave **+3.0pp** pass@1 (p=0.08) against +4.6pp for the gold-patch oracle version — selection signal without ground truth is real but weak. Not worth a v1 dependency; note it for the M8 selection work.

### Grounding mechanisms (harness-enforced, not prompt-enforced)

1. **Read-before-write invariant (hard, in C++ tool layer).** Every `edit`/`write` to an existing path requires a prior `read` of that path in the same session whose content hash matches current disk state. Stale or missing read → tool error naming the file. New files exempt.
2. **Anchor-matched edits only.** `edit` takes `old_string`/`new_string`; the harness requires `old_string` to match exactly once (or `replace_all`). Zero or >1 matches → error + re-read hint. No line-number addressing (numbers rot after any edit).
3. **Claim → evidence ledger.** Every assistant turn's factual claims about repo state must be traceable to a tool-call id. Cheap approximation: when the model states "tests pass" or "file X contains Y" in its final message, the harness checks a tool result exists that substantiates it (test-run exit 0; grep/read hit). Unsubstantiated claims get flagged in the run record as `ungrounded_claims`, not blocked (too noisy to hard-fail).
4. **Forced tool use over recall.** System prompt forbids answering repo questions from memory; the harness detects "repo-shaped" answers (mentions of file paths/symbols) with no tool call in the turn and injects a reminder. Soft nudge, hard invariant stays in tools.
5. **Verification gate before "done".** There is **no completion tool** (`06` §Core tool set): "done" is the model ending its turn without a tool call, and the gate fires then. The command to run comes from **the project's own configuration** — a declared `test_command`, or auto-detected build/test entry points — never from the model.

   That last point is the design, not an implementation detail. A model-chosen verification command is a **reward-hacking surface**: the model can nominate a command that passes trivially. Harness-owned verification means the model cannot influence what proves its own work. If no verification command can be determined, the gate reports `unverified` rather than passing — the model must not be able to reach "done" by removing the check.

   The harness runs the command itself and reads the exit code directly; a model-reported exit code is never trusted. Run on every eval and on a sampled ~10% of production runs, since a full suite per turn is expensive.
6. **Spec-then-implement for large edits.** Above a token/diff threshold (e.g. >150 changed lines), require a plan message listing files+functions to touch; the harness diffs actual change vs plan and records `scope_drift_ratio` = lines changed outside declared files / total.
7. **Ambiguity escalation.** The `ask_user` tool is always available and the system prompt sets a budget rule: if two plausible interpretations of a request would touch different files, ask. Track `questions_asked` per run — a run that ends with a large diff and zero questions on an ambiguous task is suspicious, not impressive.
8. **Self-consistency only where cheap.** For one-shot answers (e.g. "which file contains X"), sample 2 and compare; disagreement → force a tool check. Never use self-consistency as verification of code changes.

### Anti-reward-hacking checklist (concrete, implementable)

- [ ] Tests/verifiers live outside the agent's writable scope, or are hash-checked: any edit touching `tests/`, CI configs, graders, or the eval harness itself → hard-flag, require human confirmation.
- [ ] `git diff` audit per run: flag deletions of `assert`/`expect` lines, `@skip`/`xfail` additions, test-logic edits, `# noqa`, disabled warnings, edits to lockfiles or CI. Regex + AST pass, ~1ms cost.
- [ ] Never feed the verifier's source or scoring function into the model context on eval runs (METR: exposed graders → 30% hack rate).
- [ ] Success requires the *original* test suite at its *original* hash; "make tests pass" is never the goal — "make the recorded fail-before/pass-after pair hold" is.
- [ ] Time/latency claims independently measured by harness, not parsed from model output.
- [ ] Regression gate: every eval run gets a `hack_flags` array (regex + AST detectors + LLM monitor on sampled runs); monitor flags are human-reviewed before touching the leaderboard (METR: 49 flags → 15 real).
- [ ] Track "suspicious success" rate: pass with zero test execution, pass with test files modified, pass with diff outside declared scope.
- [ ] Sycophancy guard: when the user corrects the agent, the transcript records whether the agent re-verified or merely agreed. Sample-review `agreement_without_evidence` rate.

### Failure taxonomy → detection

| Failure | Detection mechanism |
|---|---|
| Hallucinated API/symbol | compile/typecheck failure; grep for symbol before use is cheap and enforcable |
| Phantom file edit | read-before-write invariant + post-run `git status` vs declared files |
| Test tampering | diff audit on test paths (above) |
| Reward hacking | hack_flags pipeline + sampled LLM monitor + human review |
| Sycophancy | agreement-without-evidence counter |
| Loop thrash | repeated identical tool calls (same args) → circuit breaker at N=3, force plan message |
| Silent scope shrink | plan-vs-diff comparison (mechanism 6); final message must enumerate unaddressed items |
| Context loss | periodic state snapshot (files touched, key facts) re-injected; detect by asking model to restate goal every K turns in eval runs |

### Minimal eval harness design

Fixture repos: 30–100 tiny deterministic repos (C++23, mixed languages later) with known-broken states and exact expected diffs/test outcomes. Seeded, offline, each runs in <60s. Run the full suite on every prompt/tool/model change.

**Per-run record (JSONL, one file per run):**

```json
{
  "run_id": "uuid", "ts": "iso8601", "suite": "mcode-fixtures-v1",
  "task_id": "fix-null-deref-003", "seed": 42,
  "model": "id", "scaffold_rev": "git-sha", "prompt_rev": "git-sha",
  "outcome": {"verdict": "pass|fail|hack_flagged|error", "exit_code": 0},
  "metrics": {"tool_calls": 14, "tokens_in": 51200, "tokens_out": 8300,
               "wall_s": 41.2, "turns": 9, "questions_asked": 0},
  "grounding": {"ungrounded_claims": 1, "stale_read_edits": 0,
                 "scope_drift_ratio": 0.0, "repeated_call_loops": 0},
  "hack_flags": [{"kind": "test_edit", "detail": "tests/t.cxx:12 assert removed",
                   "detector": "diff-ast", "reviewed": false, "confirmed": null}],
  "diff": "git-rev-or-patch-ref", "transcript": "ref",
  "verifier": {"cmd": "ctest --test-dir build", "exit": 0, "harness_reran": true}
}
```

**Assertions per run:**
1. `verifier.harness_reran == true` and exit code matches expectation (fail-before/pass-after pair recorded per fixture).
2. `hack_flags` empty (or reviewed+dismissed with reason).
3. `grounding.stale_read_edits == 0`; `ungrounded_claims` under threshold.
4. Diff touches only declared files (for scoped tasks).
5. Cost within budget band (tokens, wall time) — 3x median flags a thrash regression.

**Platform coverage.** The suite runs on all three tier-1 platforms (`24`), with a deliberate split by cost:

| Platform | Suite scope |
|---|---|
| Linux x86_64 | Full: fixtures + eval suite + extension tests |
| Windows x64 | Full: fixtures + eval suite |
| Linux aarch64 | Build + smoke |
| macOS arm64 | Build + smoke by default, full suite on PR label (CI is 10× Linux per minute) |

Platform-specific fixtures are separate and small: path handling (case folding, long paths, UNC, symlinks), atomic-replace retry under a Windows sharing violation, PTY spawn and resize, sandbox enforcement per OS, and config-directory resolution. These are the tests that catch a platform regression the shared suite cannot see.

**Aggregation:** per-fixture history enables paired comparison (which fixtures flipped pass→fail on this change) — far more sensitive than aggregate %. Track pass^k (k=3, same seed, same model) per fixture; a pass@1 rise with pass^3 flat means flakiness, not progress. Report both. LLM-judge only for final-message quality, always position-swapped, never counted in verdict.

## Traps

- **Optimizing the fixture suite.** Fixtures are in-repo and small; models will memorize them if reused for capability claims. Use them only for regression detection; use fresh tasks for capability numbers.
- **Trusting self-reported exit codes / test output pasted in text.** Always re-run or capture tool output structurally.
- **pass@k on unreliable scaffolds.** A scaffold that retries internally inflates pass@k while pass^k stays flat. Fix k semantics in the harness, not the report.
- **LLM monitor false positives.** METR: 49 flagged → 15 real. Never auto-penalize monitor flags.
- **Binary-only grading.** Per OpenAI's paper, penalize confident wrong answers and reward "I don't know"/escalation, or your evals train the agent to bluff instead of ask.
- **Contamination via docs.** Fixture task descriptions get posted/discussed; keep private split and rotate.
- **Editing tests to pass is the #1 hack;** editing *benchmarks scripts* is #2. Both are one `git diff` grep away — do the grep.

## Open questions

- Optimal sampled-monitor rate for production (non-eval) runs: 10% of runs × cheap monitor vs 100% × regex-only?
- Does anchor-matched-only editing (no fuzzy match) measurably raise task completion time on large refactors? Aider's exact-match rate (91.6%) suggests acceptable, but untested for multi-file C++ refactors.
- Can `scope_drift_ratio` thresholds be tuned per task type without becoming a gaming surface themselves?
- Cost of always-rerun verification vs sampled: measure false-"done" rate in production to decide.

## Sources

- https://www.swebench.com/ ; https://www.swebench.com/multilingual.html
- https://scale.com/blog/swe-bench-pro ; https://arxiv.org/abs/2509.16941
- https://arxiv.org/abs/2406.12045 (tau-bench, pass^k) ; https://github.com/sierra-research/tau-bench
- https://aider.chat/docs/leaderboards/ ; https://aider.chat/2024/12/21/polyglot.html
- https://metr.org/blog/2025-06-05-recent-reward-hacking/ ; https://metr.org/evaluations/gpt-5-report/
- https://metr.org/blog/2025-03-19-measuring-ai-ability-to-complete-long-tasks/ ; https://arxiv.org/abs/2503.14499
- https://arxiv.org/abs/2406.10162 (Sycophancy to Subterfuge) ; https://www.anthropic.com/research/reward-tampering
- https://arxiv.org/abs/2310.13548 (sycophancy in LMs)
- https://openai.com/index/why-language-models-hallucinate/ ; https://arxiv.org/abs/2509.04664
- https://openai.com/index/separating-signal-from-noise-coding-evaluations/
- https://arxiv.org/abs/2306.05685 (MT-Bench judges) ; https://arxiv.org/abs/2406.07791 (position bias)
- https://hal.cs.princeton.edu/gaia
- https://www-cdn.anthropic.com/bf10f64990cfda0ba858290be7b8cc6317685f47.pdf (Opus 4.5 system card; vendor self-reported)
- https://personalagentbench.com/benchmarks/webarena/ (third-party aggregator; treat rankings as [UNVERIFIED] primary)
- https://www.anthropic.com/engineering/demystifying-evals-for-ai-agents
