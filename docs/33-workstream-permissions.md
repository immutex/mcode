# Workstream A — Permissions, approvals, and the remember store

> TL;DR: The permission layer today is a gate that denies everything or allows
> everything, and it prompts nobody. This slice makes it a real engine —
> `allow` / `ask` / `deny`, four scopes, a terminal approval prompt, and a
> persisted "don't ask again" store — so the agent is usable without a `--yolo`
> flag and safe without one. **The product requirement is that the common path
> never prompts.**

## Why this slice is first

Measured against the tree, not the docs:

| Claim | Reality |
|---|---|
| `exec_policy` has `allow_argv` / `deny_argv` | **Never populated in production.** No config key reads them, no runtime path writes them; only `tests/test_tools.cxx` sets them, which is why the unit tests pass while the tool is unusable |
| A user can allowlist a command | No. The only way to run `bash` is `--yolo` |
| There is an approval prompt | There is not. `exec_policy::decide` returns `allow` or `deny`; nothing asks |
| `[permissions] deny` / `ask` are config keys | **Validated and never read.** `config.cxx` enforces that a project file may only *set* them; no consumer ever reads the result |
| `[sandbox] approval = "never" \| "on-request" \| "always"` | Documented in `22`, **never read** |
| Read outside the workspace | `workspace::resolve` **refuses**. `12`'s table says `ask` |
| Write inside the workspace | Auto-allowed. `12`'s table says `ask` with session persistence |

So the current behaviour is: **every `bash` call is denied unless `--yolo`.**
`--yolo` then allows any parsed argv -- it is checked after the deny list and
after the exec-runner refusal (`exec_policy.cxx:57-71`), so those two still
apply, but with both lists empty in practice the only surviving check is
`is_exec_runner`. That is simultaneously the most annoying possible default and
the least safe one. It is the single thing blocking `mcode` from being used on a
real repository, which is why it goes first.

## Scope

**In**

- `permission_decision { allow, ask, deny }` and a rule engine with four scopes
- Rule evaluation: `deny` → `ask` → `allow`, first match wins, managed > user > project > session
- An `approval_source` interface: terminal (interactive), headless (fail-closed), scripted (tests)
- A terminal prompt with four answers: yes once / always / no once / never
- A persisted **remember store** so "always" survives the session
- Reading `[permissions]` config (`deny`, `ask`, `allow`) and `[sandbox] approval`
- Wiring into the loop's dispatch so **every** tool is checked, not just `exec`
- Turning `workspace::resolve`'s hard refusal outside the workspace into a policy decision
- `--yolo` and `--approval <never|on-request|always>`; `--add-dir` for extra roots

**Out**

- OS sandbox enforcement (Landlock, Seatbelt, restricted token). Still deferred per `16` M6; the policy layer is what ships
- Egress proxy and network policy. `net` class is classified but nothing uses it yet
- Extension trust grants (hash-pinned, view-then-grant). That is `12` §T3 and belongs with the trust DB, which this slice does not build
- Per-domain network rules. There is no network tool yet

## Resolved: the default rule set

`12`'s table says *"Edit/create inside workspace: ask"*. Taken literally, a
coding agent prompts on every file edit — the "bothering the user" failure, and
enough on its own to make the tool unusable however good the rest is.

**Resolved: workspace-internal writes are `allow`. Prompts are for commands and
for writes outside the workspace.**

| Action | Default | Persist? |
|---|---|---|
| Read inside workspace | `allow` | — |
| **Edit / create inside workspace** | **`allow`** | — |
| Write outside the workspace | `ask` | never auto-persisted (`12`) |
| Delete outside the workspace | `deny` | — |
| Write `.git/`, `.mcode/` internals | `deny` | — |
| Read `.env`, keys, `id_rsa*`, `.aws/`, `*.pem` | `deny` | — |
| Shell: read-only builtins inside workspace | `allow` | — |
| **Shell: any write or exec** | **`ask`** | per exact parsed argv |
| Shell: network | `ask` | per exact argv |
| Unparsable or compound command | `ask` | no |
| MCP tool call | `ask` | per tool, per project |
| **Anything on the hard-deny floor** | **`deny`, always** | — |

Why a write inside the workspace needs no prompt: it is already bounded by three
things a prompt would not improve — the canonical-path boundary check
(`workspace::resolve`), the read-before-write hash invariant (`session_reads`),
and the `.git/` / `.mcode/` deny rule (`is_protected`). The prompt adds a
keystroke, not a guarantee, and git is the real undo.

This is a deviation from `12`'s table and is recorded as one. A user who wants
the stricter reading sets `[permissions] ask = ["write"]` and gets `12`'s table
verbatim — one config line, no code change, and both paths are tested.

## The hard-deny floor

**The floor is the part that survives `--yolo`.** Everything else in this
document is a question the user can answer; the floor is the small set of
actions that are not asked about at all, because the answer is always no.

`12` §Traps warns that "auto-allow without a sandbox" is the trap, and today
`--yolo` is exactly that. The floor is what keeps `--yolo` from being
indistinguishable from `--no-policy`, and it is why the flag can stay honest:
*yolo skips questions, it does not remove the floor.*

### What is on it

Small on purpose. **A false positive here is worse than a missing rule**: it
blocks legitimate work, and a user who hits one reaches for the config override
and then leaves it off. So each rule must name what it prevents, and each must be
tested against its legitimate near-miss.

| Rule | Fires on | Does **not** fire on |
|---|---|---|
| Recursive delete of a root | `rm -rf /`, `rm -rf ~`, `rm -rf $HOME`, and any recursive delete whose canonical target is a filesystem root or the user's home | `rm -rf ./build`, `rm -rf node_modules`, `rm -rf /tmp/mcode-x` |
| Privilege escalation | `sudo`, `doas`, `runas` as the first token | a path or argument that merely contains the word |
| Filesystem creation | `mkfs`, `mkfs.ext4`, and friends | `mkfs` as an argument to something else |
| Raw write to a device | `dd` whose `of=` target is a device path (`/dev/sd*`, `/dev/nvme*`, `\\.\PhysicalDrive*`) | `dd of=./image.img`, `dd of=/tmp/x` |
| Partition/format tools | `fdisk`, `diskpart`, `format` on Windows | the same words as arguments |
| Writing `.git/` internals | the existing `is_protected` deny | ordinary files under a repo |

Two design constraints on the implementation:

- **Match the canonical target, not the string.** `rm -rf /` and `rm -rf //` and
  `rm -rf /./` are the same command; a string compare misses two of them. This
  reuses `platform::canonicalize` and `workspace::contains`, which already exist.
- **The floor is checked before the allow rules and before `yolo`.** It is not a
  rule in the list; it is a separate check, because a rule can be overridden by a
  later scope and the floor must not be.

### What it is not

It is **not a sandbox**, and the code and docs must not imply it is. It is a
policy check on parsed argv and canonical paths. A command that reaches outside
those two inputs — an opaque script, a binary that deletes on its own — is not
caught. That is what the OS sandbox is for, and it does not exist yet
(`16` M6). The README and `--yolo`'s help text must say so plainly.

It is also **not a substitute for the deny list**. `permissions.deny` is the
user's own set of hard blocks and is honoured at the same level.

## Design

### Decisions and rules

```cpp
enum class permission_decision { allow, ask, deny };

// What the engine is asked about. The class comes from the registry, so an
// extension tool and a built-in with the same class are treated identically
// (12: "never on tool identity").
struct permission_request {
    std::string tool_name;
    tool_class klass;
    std::string owner;      // "" for core, extension name otherwise
    std::string resource;   // path for fs tools, parsed argv for exec, url for net
};
```

`resource` is the thing the rule matches on: a canonical path for file tools, the
canonical parsed argv for exec (`12`: "match on parsed argv, not raw strings"),
a URL host for net. A rule is `(class, pattern) -> decision`, where pattern is
exact for exec (never a prefix) and glob for paths.

### Scopes and precedence

| Scope | Source | May widen? |
|---|---|---|
| managed | `<install>/permissions.json` | yes |
| user | `%APPDATA%\mcode\permissions.json` | yes |
| project | `<repo>/.mcode/permissions.json` | **no** — `ask` / `deny` only |
| session | in-memory, from the approval prompt and CLI flags | yes (it is the user's own act) |

Evaluation is `deny` → `ask` → `allow`, first match wins across the merged rule
list. A project-sourced `allow` is **dropped at load with a warning**, not
silently honoured, because a cloned repo must not be able to widen its own
permissions (`12` T3).

### The remember store

`12`'s table has a "persist don't ask?" column with values `session`, `per
exact parsed argv`, `per project`, `no`. Three of those need a home, and only
one of them is a file:

| `12`'s value | Where it lives |
|---|---|
| `session` | In-memory only, in the engine. Cleared when the process exits — which is what "session" means |
| `per exact parsed argv` | The store's `exec` map, written on `[a]` |
| `per project` | The project store, `<repo>/.mcode/permissions.json`, written on `[a]` when the cwd is inside a repo and the rule is path-scoped |
| `no` | Never written. `[d]` still records a deny — see below |

**`[d]` ("never allow") is a session-scope deny unless the user says otherwise.**
`12` marks several rows `no` for persistence, and a permanent deny written on
one keystroke is the kind of decision a user regrets. So `[d]` denies for the
session and the prompt says so; `permissions.deny` in a config file is how a
permanent deny is expressed. The one exception is a deny the user reaches from
the `[?]` detail view, which offers to write it to the project store.

**Decision: a separate `permissions.json`, not `config.toml`.**

- `config.toml` is read-only in this tree — there is **no TOML writer**, and
  writing one to append a permission line risks clobbering the user's comments
  and formatting on a file they hand-edit.
- `permissions.json` is written by us, for us, and we already have a JSON writer
  (`json::node`, `json_write.cxx`) with sorted keys and a tested contract.
- It is trivially inspectable and deletable — "why is this allowed?" and "forget
  everything" both have obvious answers, which a nested TOML table does not.

Shape:

```json
{
  "version": 1,
  "exec":   { "git status": "allow", "npm install": "deny" },
  "paths":  { "/home/u/notes/*": "allow" },
  "tools":  { "mcp__github__create_issue": "allow" }
}
```

Keyed by canonical form, so `git   status` and `git status` are one entry and a
remembered path is the resolved path, not the spelling the model used.

**Atomic write**: sibling temp file plus rename, the same primitive
`workspace::write_file` already uses. A crash mid-write must not leave an
unreadable store, because an unreadable store would silently re-prompt for
everything (annoying) or, worse, be treated as empty and allow nothing.

### The approval prompt

Modelled on the `ask_user` prompt that already exists in `exec_tools.cxx`
(`prompt_and_read`), because a second prompt convention is a second thing to
learn and to test.

```
mcode wants to run:
    git push --force origin main

  [y] allow once   [a] always allow "git push"   [n] deny once   [d] never allow   [?] details
```

- **`[a]` persists the canonical parsed argv**, never a prefix (`12` §Traps:
  "prefix rules as security" is the named failure). `git push` remembered does
  not authorize `git push --force`; they are different argv.
- **`[d]` persists a deny**, which outranks any later allow from a lower scope.
- **`[?]`** prints the full argv, the cwd, and which rule matched. It is the
  answer to "why is it asking me this", and it is what makes the approval
  meaningful: `12`'s table requires an `ask` on anything unparsable or compound,
  and a user cannot judge a compound command they cannot see.
- Anything unrecognised re-prompts rather than defaulting. A prompt that
  guesses is a prompt that grants by accident.

### Modes, and what "not bothering the user" means concretely

| Setting | Behaviour |
|---|---|
| `--yolo` / `approval = "never"` | No prompts at all. Every `ask` resolves to `allow`, whatever it is — the user has said "do not ask me". **The hard-deny floor still applies**, and so does any `permissions.deny` rule. Yolo is not `--no-policy` |
| `approval = "on-request"` (default) | Prompt only where the rule says `ask` |
| `approval = "always"` | Prompt for everything, including reads. The paranoid setting |
| Headless (`--json`, no TTY) | An `ask` resolves to **deny**, surfaced to the model as a tool error naming the rule, unless `--yolo`. Fail closed, and say why |
| `--no-sandbox` | Not in this slice. Recorded here so it is not quietly conflated with `--yolo` |

**Yolo is not "disable everything".** It is "stop asking me". The floor is what
makes that distinction real rather than rhetorical: `--yolo` on a repository you
do not trust still refuses `rm -rf ~` and still refuses to write `.git/`. A user
who wants the floor gone has no flag for it, deliberately — that would be
`--no-policy`, and it is not a thing this design offers.

The last row matters: `12` §Traps says "auto-allow without a sandbox" is the
trap, and today `--yolo` is exactly that, because the sandbox does not exist yet.

**This is not hypothetical — the shipped help text already makes the false
claim.** `cli/exec.cxx:246` prints `--yolo  skip approval prompts (still
sandboxed)`, while `platform/seams.cxx` reports `sandbox_support::unavailable`
and `apply_sandbox` returns `unsupported` on all three platforms. So the binary
tells the user a security property holds that does not. **Fixing that string is
part of this slice, not a documentation nicety** — it is the one place the code
asserts something untrue about the sandbox, and it is a two-line change.

## Files

| Path | Owner | Change |
|---|---|---|
| `src/mcode/perm/permission.hxx` | A | `permission_decision`, `permission_request`, `rule`, `permission_engine` |
| `src/mcode/perm/permission.cxx` | A | rule merge, scope precedence, project-cannot-widen |
| `src/mcode/perm/approval.hxx` | A | `approval_source` interface, `approval_outcome` |
| `src/mcode/perm/approval_terminal.cxx` | A | the interactive prompt |
| `src/mcode/perm/approval_headless.cxx` | A | fail-closed source, used under `--json` |
| `src/mcode/perm/store.hxx` / `.cxx` | A | `permissions.json` read / atomic write |
| `src/mcode/tools/exec_policy.{hxx,cxx}` | A | folded into the engine; `exec_decision` is deleted, not kept as a second vocabulary |
| `src/mcode/tools/context.hxx` | A | **consumes** the `permission_engine*` / `approval_source*` fields Phase 0 adds; A does not add them |
| `src/mcode/agent/loop_tools.cxx` | A | the check, before the handler runs |
| `src/mcode/tools/read_tool.cxx`, `write_tools.cxx`, `search_tools.cxx` | A | outside-workspace becomes a decision, not a hard refusal |
| `src/mcode/cli/exec.{hxx,cxx}` | A | `--approval`, `--add-dir`; help text for `--yolo`. **A's exclusively** — B's `skill` command lives in `cli/skill_command.*` |
| `src/cli_commands.cxx` | **integration** | construct the engine, the source, the store |

## Steps

1. `perm/` types and the engine, with the rule merge and scope precedence.
   Unit tests first — this is pure logic with no I/O and it is where the
   security bugs live.
2. The **hard-deny floor** as a separate check, ahead of the allow rules and
   ahead of `yolo`, with its near-miss table as the test fixture. Canonical-target
   matching, reusing `platform::canonicalize` and `workspace::contains`.
3. The **default rule set** from §Resolved: workspace writes `allow`, everything
   else as the table says. This is data, so it is one table and one test.
4. `approval_source` plus the terminal and headless implementations, with a
   scripted source for tests.
5. The store: read, atomic write, and the merge into the engine's rule list.
6. Fold `exec_policy` into the engine and **delete** `exec_decision` — one
   vocabulary, not two. Port the existing argv parsing and `is_exec_runner`
   unchanged; they were reviewed and are correct.
7. Insert the check in `loop_tools.cxx`, before the handler is looked up. Log
   and publish the decision (`tool.pre_call` is already a vetoable event in
   `18`, so extensions must see it).
8. Convert the file tools' hard refusals into decisions. `is_protected` stays a
   query and keeps its deny semantics.
9. Read `[permissions]` and `[sandbox] approval` from the merged config.
10. CLI flags, help text, and the honest `--yolo` description.

## Acceptance

The test that matters, in `tests/test_permissions.cxx` plus one live run:

- **A `bash` call with no allowlist prompts, and the answer is honoured.** The
  scripted source answers "always"; the same command in a second run does not
  prompt, because the store was written and re-read.
- **A remembered allow is argv-exact.** `git status` remembered does not
  authorize `git push`. This is the bypass class `12` names, and it is the test
  that would catch a prefix rule sneaking in.
- **A deny outranks an allow** regardless of scope order.
- **A project store cannot widen.** A `.mcode/permissions.json` containing
  `{"exec": {"rm -rf /": "allow"}}` loads with a warning and the entry is
  dropped; the call still prompts.
- **Headless fails closed, and the model is told why.** With `--json` and an
  `ask`, the tool call fails with a message naming the rule and the run
  continues — the model can choose another route. This follows `ask_user`'s
  existing headless convention exactly (`exec_tools.cxx:252`), which returns an
  error result rather than aborting. **Exit 5 is for the run-level case**: the
  loop terminates because a denial left it unable to make progress.
- **`--yolo` allows an `ask` but still denies the floor.** Both halves asserted;
  the second is the one a careless implementation drops. `--yolo` plus
  `rm -rf ~` is still denied; `--yolo` plus `rm -rf ./build` is allowed.

### The floor's own tests, including the false positives

Each floor rule is tested against its legitimate near-miss. **A rule that fires
on the near-miss is a defect, not a conservative default** — it blocks real work
and trains the user to disable the check.

| Denied | Allowed (the near-miss) |
|---|---|
| `rm -rf /`, `rm -rf //`, `rm -rf /./` | `rm -rf ./build`, `rm -rf node_modules` |
| `rm -rf ~`, `rm -rf $HOME` | `rm -rf ~/scratch/project-a` |
| `sudo apt install x` | `echo sudo`, `grep sudo README.md` |
| `dd if=/dev/zero of=/dev/sda` | `dd if=/dev/zero of=./image.img bs=1M count=10` |
| `mkfs.ext4 /dev/sdb1` | `man mkfs.ext4` |
| a write to `.git/config` | a write to `.gitignore` |

The `.git` row is the one most likely to regress: `is_protected` matches the
`.git/` directory, and `.gitignore` merely starts with the same characters. A
prefix match would refuse every `.gitignore` edit, which is a file the agent
edits constantly.
- **The store survives a crash mid-write.** Kill between temp-write and rename,
  re-read: the previous contents are intact, not truncated to empty.

- **An outside-workspace read is a decision, not a hard refusal.** Today
  `workspace::resolve` refuses it outright. After this slice: with a deny rule it
  is denied; with `approval = "always"` it prompts; with the default rule set it
  prompts and the answer is honoured. All three asserted, because "convert a
  refusal into a prompt" is the change most likely to accidentally convert it
  into an allow.
- **`--add-dir` widens the boundary for one run.** A read of a file under the
  added directory does not prompt, and the same read without the flag does.
- **`approval = "always"` prompts even for an in-workspace read**, which is the
  setting's entire purpose.

Live evidence, the same way the last batch found its real defects: run a real
turn against a repository, let the model call `bash` twice, answer "always" the
first time, and confirm the second call runs without a prompt.

## Traps

- **Two decision vocabularies.** `exec_decision` and `permission_decision` must
  not both exist. Delete the first.
- **A prompt that defaults.** Every unhandled answer re-prompts. `getline`
  failing (EOF, closed stdin) is a deny, never an allow.
- **A floor rule that fires on the near-miss.** `rm -rf ~/scratch` is not
  `rm -rf ~`; `.gitignore` is not `.git/`. A rule that blocks legitimate work is
  worse than the rule it replaces, because the user disables the whole check.
- **Matching the floor against the raw string.** `rm -rf /`, `rm -rf //` and
  `rm -rf /./` are one command. Canonicalize the target first.
- **Putting the floor in the rule list.** A rule can be overridden by a later
  scope; the floor must not be. It is a separate check, ahead of the merge.
- **Persisting a prefix.** The store keys on the canonical parsed argv. A
  wildcard would be `12`'s named bypass.
- **Project scope widening.** Enforced at load, with a warning, not at use.
- **`--yolo` described as sandboxed.** It is not, because no sandbox exists. The
  help text and README must say so, or the claim is false.
- **Reading `[permissions]` before the merge.** The never-widen rule is enforced
  during merge (`config.cxx`). Read the merged result, never a raw layer.
- **Unbounded prompt loop in headless.** No TTY means no prompt at all, not a
  prompt that reads EOF forever. `ask_user` already establishes the shape: check
  `context.headless` first, return an error, never reach the prompt.
- **Conflating a denial with a fatal error.** A denied call is one failed tool
  call, not the end of the run. Only a denial that leaves the loop unable to
  progress is exit 5.
- **A rule that matches nothing but looks like it does.** An `ask` rule for a
  tool that is not registered is a typo; `mcode config check` should report
  rules that never fired at least once in the session.

## Sources

- `docs/12-security.md` — the decision table, workspace boundary, scope precedence, "match on parsed argv", the prefix-rule and auto-allow traps
- `docs/12-security.md` §Traps — "auto-allow without a sandbox", which is why the hard-deny floor exists rather than a second flag
- `docs/22-config-and-cli.md` — four scopes, the never-widen rule, `[sandbox] approval`, `--yolo`, exit code 5
- `docs/16-roadmap.md` M1 — permission UX as the two-axis `sandbox × approval` model, and the deferral of OS enforcement to M6
- `src/mcode/tools/exec_policy.{hxx,cxx}` — the existing argv parser and `is_exec_runner`, both reused
- `src/mcode/tools/exec_tools.cxx:99` — `prompt_and_read`, the prompt convention this follows
- `src/mcode/fs/workspace.cxx:262` — `resolve`, whose outside-workspace refusal becomes a decision
- `src/mcode/support/config.cxx:102` — the project-cannot-widen enforcement that already exists
- `src/mcode/agent/loop_tools.cxx` — the dispatch point the check is inserted into
