# Session analysis: a long run against DeepSeek V4 Flash

> TL;DR: Two runs of the same six-file website task — one before the fixes and one after. Ten defects surfaced, **all of them in the harness rather than the model**: every `bash` call was refused, every directory-scoped `grep` silently returned zero matches, and every `run.end` record was invalid JSON. Seven came from the run; three more came from CI, including a screen gate that had never once passed there. The model made **zero** malformed tool calls in either run, so tool-call robustness is not this model's problem. The environment reporting was.

## Method

One long task, run twice with an identical prompt, on a workspace reset to the same starting state between runs.

```powershell
./build/Release/bin/mcode.exe exec --cwd "C:/Users/mutex/site-lab" --yolo --json `
  --max-steps 60 --max-budget-usd 0.75 "<six-file website prompt>"
```

The task asks for four HTML pages, a responsive stylesheet and a script, with the instruction to read `README.md` first and follow its conventions. It is long enough to cross the context-compaction boundary and to produce tens of tool calls — the properties that make a run worth analysing.

Logs are JSONL under `%LOCALAPPDATA%/mcode/State/sessions/`. `tools/analyze_session.py` dissects one; `tools/compare_sessions.py` diffs two.

## Defects found by running

Running is what found these. A green build and a passing suite say nothing about any of them.

| # | Defect | Measured effect |
|---|---|---|
| 1 | `run.end` built invalid JSON — a stray `"` after a numeric field | Every run summary was unparseable. The run total, cache accounting and stop reason were invisible to every consumer. |
| 2 | `event_log::append` did not validate its payload | Nothing noticed defect 1. Payloads are assembled by hand as strings, so a malformed one is a silent loss. Now wrapped and counted. |
| 3 | `tool.result` recorded only `{"ok":false}` | The failure reason, the failing class and the elapsed time were dropped, so a transcript could not explain its own failures. |
| 4 | `to_extended_path` prefixed `\\?\` to a path containing `/` | Windows rejects `\\?\C:/...` outright. `grep` scoped to a directory returned `files_scanned: 0, files_skipped: 4` and `count: 0` — **a silent false negative the model could not distinguish from "no matches"**. |
| 5 | `parse_command_line` ran its metacharacter check on the *decoded* token | `grep -n -A 34 '<header>'` was refused as "compound". Nothing runs a shell — tokens become `argv` directly — so a quoted metacharacter is literal data. **All five `bash` calls in run 1 were refused**; four by this. |
| 6 | `bash` reported `ok: true` for a command that never ran | A crashed process returned `exit_code: -1073741502` with `ok: true`. `ok` means "the tool ran", which is not what a reader is asking. |
| 7 | Every budget stop reported `"budget exhausted"` | A run that spent its **step** limit printed "budget exhausted" beside `remaining_usd: 0.567925` — a record contradicting itself, in a field a reader would take as evidence the cost accounting was broken. |

Defects 4 and 5 share a property worth naming: **both turned a failure into a plausible success or a plausible refusal.** The grep returned a well-formed empty result. The parser returned a confident "this is compound". Neither was visible as a bug from the outside, and both cost the model real work.

## What the model actually does

**Tool-call correctness is not the weak point.** Across both runs: zero malformed arguments, zero truncations, zero repairs. The harness has a repair path for unparseable arguments and it never fired. Whatever else is true of this model on this gateway, its tool-call serialization held up.

**It verifies, and it writes its own tooling to do it.** In run 2 the model wrote `verify.mjs` and ran it four times, checking header and footer markup identity across pages, stylesheet and script linkage, viewport and title presence, skip-link and landmark structure, hard-coded colour, nav item count, tag balance and internal link integrity. Its output:

```
ok   header markup identical on all pages
ok   footer markup identical on all pages
ok   stylesheet linked on all pages
ok   script linked on all pages
ok   viewport + title + description on all pages
ok   skip link + main landmark on all pages
ok   no hard-coded colour in pages
ok   nav has four items on all pages
ok   tag balance scanned
ok   internal links: index.html, menu.html, about.html, co...
```

That is a structural site checker, hand-rolled, in a scratch file, because the harness does not offer one. In run 1 the same impulse produced three `python3 -c` scripts that were refused before they ran. The capability exists in the model and the harness gives it nowhere to go.

**It diagnoses its own environment failures.** In run 1, after being refused, its reasoning reads: *"Perhaps the `<` characters are seen as redirection! Yes — `<header...` looks like input redirection."* The model was right about the cause and then routed around it — dropping `<`/`>` from later commands and losing its verification step. A harness defect became a behavioural change in the model.

**It re-runs checks after edits, legitimately.** `node verify.mjs` ran four times with identical arguments in run 2, each run separated by an edit. Re-running a check after changing the thing it checks is correct behaviour, not thrashing — and it is the reason the core's thrash detector keys on *consecutive* dispatches rather than on a window.

**It refuses destructive commands.** Given an instruction to `rm assets/site.js` as a probe, it declined and read the file first to establish what it was.

**Reasoning repetition did not occur, but it is the documented failure mode of this model.** 0 periodic tails across 18 and 21 thinking blocks, and the core's repetition guard never fired. The published reports are consistent with that, and they describe the trigger precisely: the loop appears when *"the prompt is open-ended or ambiguous, or the file structure / files are not as expected"* — and this task was specific and the workspace was as described. `[VENDOR-ADJACENT]` The same report states the behaviour *"continues even if the model identifies the loop and intends to create an output"* and *"halts only on user intervention."*

That last sentence is the load-bearing one for harness design, and it validates what mcode already does. A guard that only *tells the model* it is looping cannot work — the model already knows and cannot stop. The intervention must come from outside the model, as an injected message, which is exactly what `repetition_guard.hxx` does. It is also why the `deepseek-guard` extension does not reimplement it: a notifying-only guard would be useless for this failure, and the core already has the working one.

## Measurements

### Cache behaviour

Three consecutive requests, from `model.usage` records:

| messages | effort | input | cached | hit | fresh |
|---|---|---|---|---|---|
| 1 | 3 | 1929 | 0 | 0.0% | 1929 |
| 3 | 2 | 2131 | 1920 | 90.1% | 211 |
| 5 | 2 | 2746 | 2560 | 93.2% | 186 |

Two conclusions, and the second contradicts guidance in `docs/15`:

1. The gateway does **not** report the whole prompt as cached on a first request. Cached reads are real and billed at the usual tenth. `price_cached_read` was left unset on the opposite belief; leaving it unset prices every hit at the full input rate — about 3.6x high on a run that is 90% hits.
2. **`reasoning_effort` is not part of the cache key on this gateway.** Request 2 changed effort 3 to 2 and still hit 90%. mcode schedules effort per phase (`high` for plan and reflect, `medium` for act) and that is measured to be free here, not a cold prefix per phase.

Run totals: run 1 was 807535 input / 44829 output / 791296 cached, a 98.0% hit rate. Run 2 was 1046372 / 41866 / 1037312 — 99.1%, mean per-request hit 95.4%, first request 0.0%.

### Run comparison

| | run 1 | run 2 |
|---|---|---|
| events | 163 | 191 |
| tool calls | 38 | 33 |
| `bash` calls that produced output | **0 of 5** | **6 of 8** |
| `grep` calls returning real matches | 0 of 4 | n/a |
| invalid log lines | 1 (`run.end`) | 0 |
| cost | — (summary unparseable) | $0.1821 |

The `bash` row is the whole story. In run 1 every shell call was refused before it ran; in run 2 the model's verification harness executed. The two non-output calls in run 2 were environmental — an MSYS binary the sandbox denies `\BaseNamedObjects` to, and a `cmd`/`powershell` invocation the permission engine refuses by policy — and neither is a harness defect.

## Three defects found after the session, by CI

Running the session found the seven above. Pushing found three more, and they are worth separating because none is visible from a log.

**The `tui-screen` gate only passed on a configured machine.** It spawns the real binary under a ConPTY and asserts on the screen it draws, but it inherited `%APPDATA%` — so on a clean runner `mcode` exited with "no provider configured" before drawing a frame. It had never passed on CI: the run that added it also failed it, and the failure predates this work. The report it produced was

```
[FAIL] the prompt is the last row and bare -- ''
[FAIL] the meter sits directly above the prompt -- ''
...
EOFError: Pty is closed
```

which reads as a TUI rendering defect. It was a missing config file. The gate now writes its own config into a private `APPDATA` and `LOCALAPPDATA` and sets the credential variable its fixture names, so it no longer depends on the host at all — verified by running it with the host's `APPDATA` pointed at a deliberately malformed config, where it still passes.

**The first-run message names neither the file nor the command.** On a clean machine, the entire output of running `mcode` was:

```
mcode: no provider configured; set [model] provider in config.toml
```

Exit 2, no terminal, 68 bytes on stderr. It does not say where `config.toml` is, and it does not mention `mcode setup`, which is the command that writes one. A reader with no config has no way to act on that. It now prints the resolved path, the key to set, and the command:

```
mcode: no provider configured.

  config file: C:\Users\<user>\AppData\Roaming\mcode\config.toml
  key to set:  model.provider

  run `mcode setup` to write one, or set [model] provider by hand.
```

This is the same defect an earlier report described as *"it doesn't open when I use it in PATH, just loads for a while then quits no logs"* — the message existed, but it did not lead anywhere.

**The gate then failed while reporting.** With the config fixed, the startup assertions passed on CI and the gate exited non-zero anyway:

```
[ok  ] the prompt is the last row and bare -- '>'
Traceback (most recent call last):
  ...
UnicodeEncodeError: 'charmap' codec can't encode character '\u25b8'
```

It quotes the screen it captured, the meter carries `▸` (U+25B8), and a CI runner's stdout defaults to cp1252. So the gate crashed printing its own verdict. Fixed with `sys.stdout.reconfigure(encoding="utf-8")`; `PYTHONIOENCODING` is not enough, because the environment that would set it is the one getting the default wrong. Reproduced exactly with `PYTHONIOENCODING=cp1252`.

All three of these share a shape worth naming: **a gate failed for a reason that was not a breach, and the failure looked like a product defect.** The config one read as a rendering bug, the encoding one as a crash. A gate that cannot tell you what it actually checked is worse than no gate, because it teaches you to ignore it.

## Tools worth adding

Ordered by how much work the model visibly did that a tool should have done.

1. **A structural check for HTML and CSS.** The model built one by hand, in a scratch file, and ran it five times in one session. It checks exactly the invariants a small tool could: shared markup identical across pages, stylesheet and script linked everywhere, no hard-coded colour, tag balance, internal links resolve. This is the single largest block of avoidable work observed.
2. **A scoped multi-file read or diff.** Consistency across four pages was established by reading them all and comparing by eye and by script. A "same region across these files" view would replace the script.
3. **Pipelines in `bash`.** `find … | head -100` is genuinely compound and is correctly refused today. The model wants it often. Either a documented escape hatch or a first-class "list files matching" tool closes the gap.
4. **A scratch-file convention.** The model wrote `verify.mjs`, `verify-js.mjs` and three `python3 -c` payloads into the workspace and then tried to delete them. Scratch files belong outside the tree, and the harness should say where.

## Harness improvements

- **`ok` should mean success, not "the tool ran".** Defect 6. A consumer reading `ok: true` next to `exit_code: -1073741502` has been told two different things.
- **A negative exit code should be decoded.** `0xC0000142` means the process could not start, which is a different problem from a command that ran and failed, and it is the one a model most often retries. Decoded now.
- **Nothing should be able to write an unparseable log line.** Defect 2 closed the choke point; the alternative was auditing every hand-built payload.
- **A silent empty result must not be indistinguishable from success.** Defect 4. `files_skipped: 4` was in the payload and the model still read the result as "no matches". A tool that skipped everything it was asked to scan should say so in the field a reader looks at.
- **The stop reason must name the binding constraint.** Defect 7. `exhausted()` is a disjunction and all three limits reported the same string.

## Extension: `deepseek-guard`

An extension was written for this model's documented quirks and then **cut down to one behaviour after the run showed the harness already covered the rest**:

- Reasoning repetition — `agent/repetition_guard.hxx` detects it and re-steers. Reimplementing it would double the warnings and split one rule across two places.
- Repeated identical tool calls — `thrash_detector` refuses the third consecutive dispatch. Same reasoning.

What remains is the failure the harness does not handle: DeepSeek's own tool-call markup (`<|DSML|…|>`) leaking into assistant text. It is a gateway fault, not a model-weight one — the serving stack must parse the DSML encoding explicitly, and without that parser the markup passes through verbatim. The rate is not marginal: one report measured it in **roughly 1 in 4 tool-bearing streaming runs** for `deepseek-v4-flash`, with both the full-width `｜` (U+FF5C) and ASCII `|` variants observed. In the captured leak the structured `tool_calls` entry was present but degenerate — `arguments: "{}"` — while the real call lived only in the leaked text, so a client trusting `tool_calls` gets an argument-less call and a client rendering `content` gets sentinel garbage. Either way the turn is broken.

The extension notifies once per turn and counts occurrences; it rewrites nothing and vetoes nothing. It declares no permissions.

**It did not fire on either run** — no leak occurred — so it is verified to load and to be wired, not to detect. Given the reported rate, a longer sample would settle it; one session is not that sample.

A first version of this extension warned on any repeated `(tool, args)` pair within a window. Running it produced **14 false warnings in one session**, every one a legitimate `node verify.mjs` re-run after an edit. That version also assumed `tool.result` carried a failure signal; it does not, because `ok` means "the tool ran". The measurement deleted the feature.

## Traps

- **`bash` calling an MSYS binary fails on this machine.** `grep`, `rm` and `sed` from Git's `usr/bin` die with `NtCreateDirectoryObject(...) 0xC0000022` because the sandbox denies the named object they create. Environmental, not a harness bug — and the reason the dedicated `grep`/`glob`/`read` tools matter.
- **`--max-steps` binds before `--max-budget-usd` on this task.** 60 steps, $0.18 of $0.75. A budget that looks generous may never be the constraint that stops the run.
- **A subagent must not build.** Parallel builds race on `.obj`; one build at a time.

## Open questions

- Does the DSML leak actually occur on this gateway? It has not been observed. Until it is, the guard is insurance, not a fix.
- Is the model's hand-rolled verifier a signal that a structural-check tool would be used, or that this model prefers writing scripts? One model, one task.
- The step limit bound before cost on a 60-step task. Is 60 the right default, or is the task simply larger than the default was sized for?

## Sources

All URLs retrieved 2026-10-08.

- Session logs: `%LOCALAPPDATA%/mcode/State/sessions/`, run 1 `762774d9817e8dc3-1791497734229.jsonl`, run 2 `762774d9817e8dc3-1791500388265.jsonl`
- `tools/analyze_session.py`, `tools/compare_sessions.py` — the two analysis passes over those logs
- `docs/15-model-layer.md` — provider behaviour and pricing
- `docs/28-b-measurements.md` — the measurement-doc format this follows
- https://github.com/cline/cline/issues/13348 — "api.cline.bot: DeepSeek V4 DSML tool-call markup leaks into content (no DSML extractor on serving path)"; source of the ~1-in-4 rate, the degenerate `arguments: "{}"` capture, and the U+FF5C/ASCII variant note
- https://github.com/vllm-project/vllm/issues/51914 — "[Bug] DeepSeek-V4-Flash-0731 intermittently emits malformed DSML tool-call start wrapper on v0.27.1 + DSpark"; the same family, a corrupted opening wrapper
- https://github.com/NousResearch/hermes-agent/issues/78807 — "[Bug]: DeepSeek V4 Flash 0731 infinite reasoning loop"; source of the trigger conditions, the "continues even if the model identifies the loop", and "halts only on user intervention"
- https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash-0731/discussions/39 — "Reasoning loops"; independent reports of the same
- https://www.mindstudio.ai/blog/deepseek-v4-1-flash-hands-on-test — hands-on coding and reasoning test; independent throughput and multi-file observations
- https://www.reddit.com/r/LocalLLaMA/comments/1ve8fel/did_anyone_notice_odd_reasoning_loops_with/ — "The last 40k of reasoning was just repeating around 15 lines over and over again"; retrieved through a search index, the page itself refuses non-browser clients
