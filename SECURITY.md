# Security Policy

> TL;DR: Report vulnerabilities privately through
> [GitHub Security Advisories](https://github.com/immutex/mcode/security/advisories/new),
> never in a public issue. This is a coding agent that reads untrusted content
> and runs commands, so its trust boundary is the point of the design and worth
> reading before you report.

## Reporting a vulnerability

**Do not open a public issue.** Use one of:

- **Preferred:**
  [private vulnerability reporting](https://github.com/immutex/mcode/security/advisories/new).
  This opens a draft advisory only the maintainer can see, and is the channel
  that produces a CVE and a coordinated release if one is warranted.
- **Email:** immutexx@gmail.com, if you cannot use GitHub.

Include what you can of the following. The first three are the ones that most
often decide whether a report is actionable.

1. **Version and platform**: `mcode --version` prints both. A report against an
   unknown build cannot be reproduced.
2. **A minimal reproduction.** The exact command, the exact workspace content,
   and what happened.
3. **Which layer you believe is at fault.** The three layers below are
   independent, and knowing which one is claimed to be broken saves a round trip.
4. **Impact**: what an attacker gains, and what preconditions they need.
5. **A suggested fix**, if you have one. Not required.

### What to expect

This is a small project without a security team, so the honest commitment is
about process rather than a service level:

| Stage | Target |
|---|---|
| Acknowledgement | 7 days |
| Initial assessment (is it in scope, which layer) | 14 days |
| Fix or a documented decision not to fix | 90 days |

If you have not heard back in 14 days, ping the advisory thread. A missed
acknowledgement is a mistake, not a decision.

Please give a reasonable window to fix before public disclosure. Credit is given
in the advisory and the release notes unless you ask otherwise.

## Scope

### In scope

Anything that breaks one of the three boundaries the design claims to enforce.
That is the interesting class, because a bug here means the documentation is
wrong, not merely that a feature is imperfect.

**Layer 1 — the permission engine.** The deterministic gate that decides what the
agent *asks* to do.

- The **hard-deny floor** being bypassed. It is meant to be unoverridable, ahead
  of every rule, ahead of `--yolo`, and ahead of a permissive default. A floor
  entry that can be reached is a finding.
- **Argv matching** being fooled: a wrapper, a flag cluster, a quoting trick, or
  command substitution that hides a destructive subcommand inside something that
  reads as benign. Nothing runs a shell, so a shell-metacharacter parse is not a
  boundary — but a *parsed argv* that does not name the program that runs is.
- **Path comparison** being fooled: symlink escapes, junctions, `..` handling,
  UNC paths, or case folding that lets a path read as inside the workspace when
  it is not.
- **Project scope widening authority.** A project-sourced config must only be
  able to add `ask` or `deny`. If a repository can widen a rule or a sandbox key,
  that is a finding.

**Layer 2 — the OS sandbox.** What a running process *can* do.

- Escape from the Windows low-integrity token, the Job Object, or the AppContainer
  path.
- Escape from the Linux Landlock ruleset or the seccomp filter, including a
  bypass that the documented ABI handling does not cover.
- Escape from the macOS Seatbelt profile, or a profile that is escapable because
  it is not deny-default.
- A **write escaping the workspace** despite the sandbox being active.

**Layer 3 — the extension VM capability boundary.** What a loaded extension can
reach.

- A reachable capability the manifest did not declare: filesystem, process
  execution, network to a host outside the declared allowlist, or a credential
  outside the declared set.
- An escape from the VM: a host reflection path, a bytecode bypass, a
  `readonly`-table bypass, or a way to reach the host C API.
- Cross-extension data theft through the shared VM global state.

**Supply chain.** The published archives and the install path.

- A published release asset that does not match the `SHA256SUMS` in the same
  release.
- A code path in `install.ps1` or `install.sh` that executes before verifying the
  checksum.
- A dependency compromise that mcode's own code does not defend against.

### Out of scope

These are real problems that mcode does not claim to solve. Reporting them is
welcome as a **public issue** rather than an advisory.

- **Prompt injection itself.** The model can be manipulated. The design bounds
  what the manipulation can *do*; it does not prevent it.
- **Exfiltration through an allowlisted channel.** Anything on the egress
  allowlist can carry data out in a URL or a package name. An allowlist is not
  DLP.
- **Content sent to the model provider.** The workspace content you ask about
  leaves the machine by design.
- **A user approving a destructive action.** The human is the last and most
  fallible line, and the approval prompt is not a security boundary.
- **Writes inside the workspace.** Permitted by design.
- **A memory-safety defect in Luau.** This is a full escape from the agent
  process, and it is the residual risk any in-process VM carries. It is worth
  reporting upstream; it is not an mcode boundary bug, and the design says so.
- **Denial of service by resource exhaustion** on your own machine, by your own
  command.
- **Anything requiring an already-compromised machine**, a malicious local
  administrator, or physical access.
- **The extension VM described as a sandbox.** It is an in-process capability
  boundary. That distinction is documented and deliberate.

## What ships today

Accurate as of `0.0.1`. Read this before assuming a documented control is
enforced — the design docs describe the target, and this section describes the
tree.

| Control | State |
|---|---|
| Hard-deny floor | **Shipped** (`src/mcode/perm/floor.cxx`) |
| Permission engine, four scopes, argv matching | **Shipped** (`src/mcode/perm/`) |
| Secret deny-read list (`.env`, `*.pem`, `id_rsa*`, `.aws/`) | **Shipped** (`src/mcode/perm/rules.cxx`) |
| Environment scrubbing for spawned processes | **Shipped** (`src/mcode/proc/process.cxx`) |
| OS sandbox, all three platforms | **Shipped** (`src/mcode/platform/sandbox_*.cxx`) |
| Extension VM capability boundary, manifest permissions | **Shipped** (`src/mcode/ext/api_gate.cxx`) |
| Run-scoped temp directory, narrowly granted | **Shipped** (`src/mcode/platform/seams.cxx`) |
| **Hash-pinned trust grant for repo-shipped extensions** | **Not shipped** — see below |
| Untrusted out-of-process extension tier | **Not shipped** (documented non-goal) |
| Network egress proxy with a domain allowlist | **Not shipped** |

> [!WARNING]
> **Repository-local extensions are loaded without a trust prompt.**
> `default_roots` in `src/mcode/ext/loader.cxx` includes
> `<workspace>/.mcode/extensions`, and every extension found there runs its
> `init.luau` at session start. `docs/12` specifies a hash-pinned grant gate for
> exactly this case (threat T3, the Neovim `exrc` problem), and that gate is
> **not implemented**.
>
> The practical consequence: **cloning an untrusted repository and running mcode
> in it executes that repository's extension code.** Treat a `.mcode/extensions`
> directory in a repository you have not read as you would a build script.
> `docs/12` §Repo-shipped extensions is corrected to record this.

If you are reporting against a control in the "Not shipped" rows, it is a
feature request rather than a vulnerability, and a public issue is the right
place for it.

## Hardening

The controls below exist and are worth using. None of them is a substitute for
reading a repository you are about to run an agent in.

- **`--ask`** restores prompting. An interactive session defaults to permissive
  because a human is present and sees the boundary printed on entry; a headless
  run defaults to conservative and fails closed.
- **`--yolo` does not disable the hard-deny floor.** It skips approval prompts
  only.
- **`--no-extensions`** for headless and CI runs, where nothing should be
  loading code from the workspace.
- **`/undo` and `/rewind`** restore files the run changed, from a content-addressed
  snapshot store. This bounds a bad edit; it does not bound an exfiltration.
- **`mcode exec --json`** emits a typed event stream, so a CI run can gate on
  what the agent actually did rather than on its summary.

## For contributors

Security-relevant code has review requirements beyond the general rules in
[`CONTRIBUTING.md`](CONTRIBUTING.md):

- **A boundary change needs a test that fails without it.** Not a smoke test of
  the happy path.
- **Never satisfy a check with a no-op.** Fix the cause.
- **Fail closed.** Missing input, unreadable state, or an unexpected error
  hard-fails rather than proceeding with a permissive default.
- **A recovery path must not turn a failure into a success.** Zero is not neutral
  when zero is the success encoding.
- **State the layer in the commit message.** A commit that touches the floor, a
  sandbox, or the VM boundary should say which claim it affects and what proves
  it still holds.
- **`docs/12` owns the trust model.** A change to a boundary updates that doc in
  the same commit. Where code and docs disagree, the disagreement is a bug in one
  of them and must be resolved rather than left.

## References

- [`docs/12-security.md`](docs/12-security.md): the threat model, the three
  layers, the trust tiers, and an explicit list of what sandboxing does not
  protect against
- [`docs/24-cross-platform.md`](docs/24-cross-platform.md): the per-platform
  sandbox mechanisms and their measured limitations
- [`docs/39-workstream-sandbox.md`](docs/39-workstream-sandbox.md): the sandbox
  workstream, including the confinement probes
- [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md): what mcode embeds and under
  which license
