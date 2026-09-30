# Workstream C — The OS sandbox

> TL;DR: The permission engine is a **policy gate**: it parses the argv,
> canonicalizes the target, and decides. It is not isolation. `apply_sandbox`
> returns `unsupported` on all three platforms, so `--yolo` means "I trust this
> repository", and a binary that deletes on its own is not caught by anything.
> This slice implements the seam for real — Windows restricted token + Job
> Object, Linux Landlock + seccomp, macOS Seatbelt — plus a per-platform egress
> deny, and makes every claim about it verifiable.

## Why this slice

`docs/12` is unambiguous that this is the highest-value control, and the tree
has been honest about the gap since the first batch:

| Claim | Reality |
|---|---|
| `apply_sandbox( profile )` exists | Yes, and returns `unsupported` on all three platforms |
| `sandbox_support_level()` reports honestly | Yes — it says `unavailable`, which is the one part that already works |
| `--yolo` still sandboxes | **It does not.** The help text said so until the second batch fixed the string |
| A spawned command is confined | No. `bash` runs with the user's full rights |
| Network egress is controlled | No. Nothing polices it |
| The `Sandbox` seam is implemented per `24` | Interfaces only |

So the security model today is: parse the command, check it against a rule list,
and trust the result. The hard-deny floor catches the named catastrophic cases
and is tested against its near-misses — that work is real and stays — but it is
a list, and a list is defeated by anything not on it.

## Scope

**In**

- `apply_sandbox` implemented for all three platforms behind the existing seam
- Windows: `CreateRestrictedToken` + `CreateJobObjectW` (kill-on-close, UI and
  memory limits) + an ACL write boundary
- Linux: Landlock with **runtime ABI detection** + seccomp
- macOS: Seatbelt via `sandbox_init_with_parameters()`, `(deny default)` with
  `(import "system.sb")`
- A per-platform egress deny scoped to the sandbox identity
- Spawn-with-profile in `proc/`, and the exec path using it
- Honest capability reporting end to end: seam, help text, README

**Out**

- AppContainer (Windows tier 2). Tier 1 needs no admin; tier 2 is default-deny
  on network and **special-cases loopback**, which collides with a future
  egress proxy. Recorded as deferred, not forgotten
- `bubblewrap`. Broken out of the box on stock Ubuntu 24.04 and setuid mode was
  removed; Landlock is primary and needs no helper
- The egress **proxy** with a domain allowlist. That is M6's other half and it
  needs a listening process; this slice does process-level network *deny*
- Sandboxing the harness's own process. The profile applies to spawned children
- Trust grants for project extensions. Separate M6 item
- Any change to `perm/`, `ext/`, `tui/`

## Design

### The seam is already right — implement behind it

`sandbox_profile` carries `read_paths`, `write_paths` and `allow_network`. That
shape is sufficient and should not change. What changes is that the three
`#if` branches stop returning `unsupported`.

**`sandbox_support_level()` must keep telling the truth per platform and per
runtime.** It is the only thing standing between a user and a false belief, and
it already reports `unavailable`. After this slice it reports what is *actually
enforced on this machine* — which is not a constant, because Landlock's ABI is
a runtime property of the kernel.

### Windows — restricted token + Job Object

The tier that needs no administrator.

- `CreateRestrictedToken` with `DISABLE_MAX_PRIVILEGE`, a deny-only SID set,
  and the user's logon SID as the sole restricting SID. **A restricted token
  without a restricting SID is not a restriction.**
- `CreateJobObjectW` with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`,
  `JOB_OBJECT_LIMIT_ACTIVE_PROCESS`, and memory limits; assign the child to it.
  Kill-on-close is what guarantees no orphan survives the harness.
- The write boundary is an ACL: grant the sandbox SID write only under the
  workspace, deny elsewhere. This is the part that is easy to get subtly wrong —
  an ACL that grants on a parent directory grants on everything beneath it.
- `PROC_THREAD_ATTRIBUTE_*` for the token, not `CreateProcessAsUser` — no
  `SeAssignPrimaryTokenPrivilege` required, which is the difference between a
  tool a user can run and one that needs an elevated prompt.

**This is the tier that can be verified on the build machine**, so it is built
and proven first.

### Linux — Landlock, with ABI detection as a hard requirement

- `landlock_create_ruleset` / `landlock_add_rule` / `landlock_restrict_self`,
  applied with `no_new_privs` set first.
- **Runtime ABI detection is mandatory, not defensive.** RHEL 9.6 is ABI 5 on a
  5.14 kernel; assuming the header's ABI produces a ruleset the kernel silently
  rejects, which is a sandbox that does not exist. Query the ABI, handle each
  version's feature set, and **hard-fail when the kernel reports an ABI the code
  does not know** rather than applying a partial ruleset.
- seccomp in filter mode for the syscall denials Landlock does not cover.
- Two documented limits, because they are real: Landlock grants **ports, not
  hostnames**, and pre-opened file descriptors bypass rules. Say so where a user
  will read it.

### macOS — Seatbelt, deny-default only

- `sandbox_init_with_parameters()` in-process: no exec wrapper, no stderr
  warning on every run.
- `(deny default)` with `(import "system.sb")`. **`(allow default)` profiles are
  structurally escapable** — a deny-default profile is mandatory, not a
  preference.
- `sandbox-exec` is deprecated (2017), warns on every run, and Apple has
  published no replacement for CLI process sandboxing. It is a documented
  fallback, not the primary path.
- Profiles are a small static set, not generated per command. A generated
  profile is a parser, and a parser for a security policy is a vulnerability.

### Egress deny

Per platform, scoped to the sandbox identity, denying by default when
`allow_network` is false:

- Windows: WFP filters scoped to the sandbox SID. **Needs admin to install** —
  so `sandbox_support_level()` must report network denial as a *separate*
  capability from filesystem confinement, because on Windows they have different
  privilege requirements. One enum cannot express "confined but reachable".
- Linux: Landlock's network rules where the ABI supports them; otherwise a
  seccomp deny on `connect`/`socket`.
- macOS: `(deny network*)` in the Seatbelt profile.

That capability split is the design point of this section. A single
`sandbox_support` enum reporting "supported" while network is open would be a
false claim, and this slice exists to remove those.

### Honest reporting is a deliverable

Three places must agree, and a test should assert the first two:

1. `sandbox_support_level()` / `sandbox_mechanism()` — what is enforced
2. `--yolo`'s help text and `README` §Non-obvious constraints — what the flag
   means now that a sandbox exists
3. The docs that describe the boundary

`docs/12`'s open question — *"is an env-proxy fallback honest enough to
document, or must Windows Tier 1 declare network isolation best-effort?"* — is
answered by the capability split above: declare it best-effort, and say so in
the enum rather than in a footnote.

## Files

| Path | Owner | Change |
|---|---|---|
| `src/mcode/platform/sandbox_windows.cxx` | C | restricted token, Job Object, ACL boundary, WFP deny |
| `src/mcode/platform/sandbox_linux.cxx` | C | Landlock + ABI detection, seccomp |
| `src/mcode/platform/sandbox_macos.cxx` | C | Seatbelt profile, `sandbox_init_with_parameters` |
| `src/mcode/platform/seams.{hxx,cxx}` | C | dispatch the three; the capability enum gains the network axis |
| `src/mcode/proc/process.{hxx,cxx}` | C | spawn with a profile applied to the child |
| `src/mcode/proc/session.{hxx,cxx}` | C | the long-lived child path takes a profile |
| `src/mcode/tools/exec_tools.cxx` | C | the exec path builds a profile and applies it |
| `tests/test_sandbox.cxx` | C | capability reporting, confinement probes |
| `tests/test_sandbox_egress.cxx` | C | network denial where supported |

## Steps

1. **Split the capability enum** so filesystem confinement and network denial
   are reported separately. Everything else depends on the reporting being
   honest first.
2. **Windows**: restricted token + Job Object. Verify locally with probes —
   a write outside the profile must fail, a write inside must succeed, and no
   child survives the parent.
3. **Linux**: Landlock with ABI detection and seccomp. Compile-verified by the
   gate, executed by CI.
4. **macOS**: the Seatbelt profile. Compile-verified, CI-executed.
5. **Egress deny** per platform.
6. Wire the exec path to build a profile from the workspace plus `--add-dir`,
   and to report the mechanism in `--verbose`.
7. Reconcile the help text, the README and `docs/12`.

## Acceptance

- `sandbox_support_level()` and `sandbox_mechanism()` report what a **probe**
  observes on each platform: a denied write is denied, an allowed write
  succeeds. A capability claim with no probe behind it is the defect this slice
  removes.
- A spawned child cannot write outside the profile's `write_paths`.
- A spawned child cannot read outside its `read_paths`.
- `allow_network = false` blocks egress where the platform supports it, and the
  capability enum says so where it does not.
- No child process survives the harness: kill the parent, assert the child is
  gone.
- **Landlock ABI detection**: with a stubbed ABI the code does not know, the
  call hard-fails rather than applying a partial ruleset.
- The `--yolo` help text, the README, and `sandbox_support_level()` agree —
  asserted, not reviewed.
- On a platform where the sandbox is unavailable, the failure is **loud and
  named**, and the run proceeds with the permission engine alone rather than
  pretending.

Live evidence: the batch acceptance run in `36`, step 6 — a command inside the
sandbox reports enforced, and a write outside the profile fails.

## Traps

- **A restricted token with no restricting SID.** Not a restriction.
- **Assuming the Landlock ABI.** RHEL 9.6 is ABI 5 on a 5.14 kernel. Query it,
  and hard-fail on the unknown rather than applying a subset.
- **An ACL that grants on a parent.** It grants on the whole subtree.
- **`(allow default)` Seatbelt profiles.** Structurally escapable. Deny-default
  or nothing.
- **One capability enum for two capabilities.** Windows filesystem confinement
  needs no admin; its WFP network deny does. A single "supported" would be a
  false claim.
- **Claiming a sandbox where the platform refused.** Fail loud, name the reason,
  and continue with the policy engine — do not silently downgrade.
- **Forgetting `no_new_privs`** before `landlock_restrict_self`; the ruleset is
  rejected.
- **Testing only that `apply_sandbox` returns success.** Success is not
  confinement. The test is a probe that attempts the forbidden operation.
- **Leaving a child unassigned to the Job Object.** It survives the parent.

## Sources

- `docs/12-security.md` — the per-platform decision table, the egress argument, the Windows network open question
- `docs/24-cross-platform.md` — minimum OS versions, Landlock ABI notes, Seatbelt deprecation, ConPTY floor
- `docs/16-roadmap.md` — M1's deferred sandbox and M6's hardening deliverable
- `docs/33-workstream-permissions.md` — the policy layer this complements, and its explicit "not a sandbox" statement
- `docs/32-second-batch.md` — the `--yolo` help-text correction, and why the string mattered
- `src/mcode/platform/seams.hxx` — the existing `sandbox_profile` and capability enum
