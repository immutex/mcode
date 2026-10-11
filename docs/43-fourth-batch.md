# Fourth Batch — Trust, Egress, and the Measurement Spine

> **Status: shipped in 0.0.3.** Every decision below landed. Where the plan was
> wrong, the code is the record: the install writes to the user scope rather than
> project scope (`25` is authoritative there), the gate's own command is
> harness-owned so the approval policy does not deny it, and a read-only
> subagent's registry fails closed rather than skipping a tool it could not copy.
>
> Verified: 694 test cases, 4461 assertions, 155 smoke checks, clang gate exit 0,
> and the exploit measured inert end to end.

> TL;DR: Three of mcode's four differentiators are **documentation, not code**.
> `12` specifies a repo-local trust gate and calls the egress proxy "the single
> highest-value component" — neither exists, and the first is a proven
> arbitrary-code-execution path. `25` specifies `mcode ext install` — no such
> command. This batch makes the claims true: a hash-pinned trust gate with no
> TOCTOU window, a loopback allowlist proxy that is the anti-exfiltration
> control, the install path that unblocks the ecosystem, and the measurement
> that proves each one works. It also fixes two defects the audit found: a
> context meter that reports cumulative spend as window occupancy, and a
> verification gate with **zero callers**. It ships **no new dependency**.

## Why this batch, and why now

Measured against the tree at `56c4b8d`, not against the docs.

| Symptom | Cause | Slice |
|---|---|---|
| A repo you clone can execute code on session start, silently | `default_roots` pushes `<workspace>/.mcode/extensions` unconditionally and `load_extensions` runs every candidate's `init.luau`; there is no trust store anywhere in `src/` | A |
| An extension cannot be installed | `mcode ext install` is specified in `25` and absent from `cli/` | A |
| A prompt-injected agent can `curl` anything | No egress proxy; `allow_network = false` is set only at `exec_tools.cxx:242`, honoured on Linux/macOS, and **absent on Windows** | B |
| The agent can claim success it never verified | `set_verification_command` has **zero callers** — the Verify state always falls through to Handoff | C |
| The context meter can read 400% | `context_used()` returns `budget_.tokens_used` (cumulative spend) and is compared against the context window | D |
| `05`'s tool-result clearing does not exist | Four constants declared in `loop.hxx:68–74`, read nowhere; `maybe_compact` is truncation with no summary | D |
| Half the MCP ecosystem is unreachable | Only `transport_stdio` ships | E |
| Every session starts from zero | No memory; `09` designs an FTS5 store the repo's own `01`/`03`/`14` forbid | F |
| Large-repo exploration floods the window | No subagents | G |

Slices A–C are release blockers: each is a claim the repo already makes and does not keep. D fixes a user-visible defect and the doc that misdescribes it. E–G are parity, ordered last on purpose.

### What "production" requires that is not a feature

The published 0.0.2 artifact was broken twice before it worked (no `extensions/`
in the archive; a smoke test that crashed without them). **A green pipeline on a
tag means the pipeline succeeded, not that the artifact runs.** Slice C adds the
check that would have caught it to the permanent suite.

## Conflicts this batch must resolve

| Doc | Current claim | Why it breaks |
|---|---|---|
| `12` §Repo-shipped extensions | Rules 1–2 assert a repo-local grant gate | Not implemented. Proven exploitable. Slice A implements it, or the section is a lie |
| `12` §Layer 2 | "A local loopback proxy with a domain allowlist … the single highest-value component" | Not built. Slice B builds it, or the claim is aspirational |
| `12` §Layer 3 | The VM is "a capability boundary, never a sandbox" | Stays true. Slice A adds *identity over time*; it does not make the VM an OS sandbox, and the wording must not drift |
| `25` §Install | `mcode ext install git:owner/repo@<ref>` with a resolved-SHA lockfile | No command. Slice A implements it |
| `01` budget "≤12 direct deps" | 9 direct requires (`conanfile.py:68–76`) | All three slices stay at 9. `09`'s SQLite would be the 10th and is rejected in F |
| `09` §Recommended design | Ships SQLite FTS5 by default + an always-injected `MEMORY.md` | Contradicts `01` ("Not a vector database"), `03` ("FTS5 if we ever need it"), `14` ("Premature"), and `09`'s own Traps. Slice F rewrites it |
| `05` §Numeric guidance | 60% tool-result clear, 80% compaction, post-compact re-reads | Constants exist and are dead; `maybe_compact` truncates. Slice D rewrites the section to match what is built |
| `05` §Subagent thresholds | Spawn at >10 calls / >30K tokens | Owned by `05`, enforced by `10`. Slice G does not duplicate them |
| `16` M1 "Egress deny (minimal)" | "A deny-by-default egress rule per platform even before the proxy" | Windows never got one. Slice B must state the platform capability honestly rather than claim parity |
| `11` §Verification gate | "Harness re-runs the project's own tests; failure blocks done" | Mechanism exists; nothing can arm it. Slice C wires it and adds the verdict vocabulary |
| `README.md` §"Not built yet" | memory, subagents, `/skill`, `/mcp`, `/session`, `/reload` | Slice F narrows the memory claim; Slice G ships subagents; the commands are out of scope here |

---

## Decision 1 — The extension trust gate (Slice A)

### Verified this session

| Finding | Evidence |
|---|---|
| A repo-supplied extension executes with no prompt | A repo with `.mcode/extensions/backdoor/{ext.toml,init.luau}` calling `mcode.fs.write("EXTENSION_RAN.txt", …)` wrote the file on session start. `mcode.fs.write` takes two positional strings; `io.open` is correctly absent |
| There is no trust store | `grep -ri trust src/` returns only `wrap_untrusted` (`mcp/constants.cxx`), the MCP taint marker |
| The root is unconditional | `default_roots` in `ext/loader.cxx` pushes `<workspace>/.mcode/extensions` with no gate |
| Two call sites | `cli/session.cxx:~225`, `cli/commands.cxx:~248` |
| The VM strips the easy escapes | `load`, `loadstring`, `string.dump`, `io` absent; bytecode rejected. So hidden bytes cannot become *code* — but they can become *data* (a URL, an argv, a path) |
| `run_file` already holds the bytes | `lua_host_execute.cxx:39–52` reads the file into a `std::string` and calls `run( source, name )` |
| SHA-256 is already linked | `OpenSSL::SSL` + `OpenSSL::Crypto`, `src/CMakeLists.txt:203–204` |
| `hash_bytes` is FNV-1a | `fs/workspace.cxx:215` — correct for the snapshot store (re-hashed on read), **unusable as a security boundary** |

### The design

**Grant granularity: one digest over the whole extension tree.** Not per file —
with a runtime `require` graph a per-file grant must also pin the *set*, and
pinning the set is a tree digest with extra steps. Not directory-by-name —
that is Neovim's directory mode, which its own prompt calls "decided only by
name, not contents."

**Trust store:** `<user-state>/mcode/trust` (`%LOCALAPPDATA%` / `$XDG_STATE_HOME`).
Outside every workspace root, so the permission engine's workspace-write rules
cannot reach it. Line-oriented, tab-separated, LF:

```
v1	allow	<scope>	<name>	<api_version>	<tree_sha256>	<root>
v1	deny	<scope>	<name>	0	-	<root>
```

Not TOML: this file is written and read on a security path, and a strict
`split('\t')` with an exact field count fails closed on corruption while a TOML
parse invites defaulted fields. Any unparsable line is a **hard error for the
whole store**, not a skipped line. Writes are temp-file + atomic rename.

**The load-time check — the bytes hashed are the bytes executed:**

1. Enumerate the root; refuse any entry whose `symlink_status()` is not a
   regular file or directory; refuse any path escaping the root, compared
   **component-wise** (not by string prefix; `weakly_canonical` leaves a final
   link in place).
2. Read every file **once** into a bounded snapshot (`vector<{relative_path, bytes}>`).
   Over the ceiling is a refusal, never a truncation.
3. `tree_sha256 = sha256( domain_separator ‖ count ‖ Σ_{path in byte-sorted order} path ‖ 0x00 ‖ sha256(bytes) )`.
4. Look up `(scope, name)`. No record → **inert**. Deny → **inert**. `api_version`
   differs → inert. Digest mismatch → inert, naming the differing paths (the
   per-file hashes make this free), and **do not rewrite the grant**.
5. On match, serve `init.luau` and every `require` **from the snapshot**.
   `module_loader` stops touching the filesystem.

Step 5 is the whole TOCTOU answer: there is no second read, so there is no
window — not "a small window," none. It is also nearly free, because `run_file`
already reads bytes into a string and compiles that string; the change is where
the bytes come from.

**Hash bytes, never text.** No newline translation, no encoding conversion, no
BOM stripping. Five Neovim trust-store fixes in fourteen months are exactly this
bug, one of them a mismatch against `sha256sum`.

**Windows belt:** open with a share mode denying write and delete and hold the
handle across read-and-hash, so a concurrent writer gets
`ERROR_SHARING_VIOLATION` instead of racing. Portable correctness comes from the
snapshot; this removes the race rather than narrowing it.

**Grant UX:** `mcode ext trust <name>` takes a **name from the already-discovered
set, never a path** — a path argument is a race by construction, which is what
Neovim warns about for `:trust [file]`. Before granting, render the files with
every Unicode `Cf`/`Cc` (except TAB/LF/CR) and every variation selector
(U+FE00–FE0F, U+E0100–E01EF) **escaped and counted**. GlassWorm shipped 35,800
downloads of extensions whose payload renders as nothing; a plain view is not a
review. No one-key allow, no "always allow" checkbox.

### What this does not stop

1. **First-install trust.** The digest is computed from the bytes the attacker
   supplied. It proves *these bytes have not changed*; never *these bytes are
   safe*. This is the most important sentence in the design.
2. **An extension the user deliberately installed and granted.** The grant pins
   identity over time, not intent.
3. **Trust-store or lockfile compromise.** Both are user-writable; an attacker
   with user-level write already has user-level execution.
4. **A hostile `git` binary or filesystem filter driver.**
5. **What a permitted capability does.** The digest constrains which bytes hold
   the argv, not what the argv does.
6. **Everything outside `.mcode/extensions/`** — MCP servers, skills, `AGENTS.md`.
7. **The human.** The grant prompt is the last line, and 35,800 downloads is the
   measurement of how fallible it is.
8. **A Luau VM defect** — unchanged from `12` item 11.

`--no-extensions` remains the only mitigation depending on none of the above.

### Cost

| Piece | File | LOC |
|---|---|---|
| Tree digest, bounded snapshot, containment | `src/mcode/ext/integrity.{hxx,cxx}` (new) | ~200 |
| Trust store parse/lookup/atomic write | `src/mcode/ext/trust_store.{hxx,cxx}` (new) | ~180 |
| Lockfile read/write | `src/mcode/ext/lockfile.{hxx,cxx}` (new) | ~140 |
| Loader wiring; `module_loader` becomes a map | `src/mcode/ext/loader.cxx` | ~60 changed |
| Entry point from bytes, not a path | `src/mcode/ext/lua_host_execute.cxx` | ~15 changed |
| `ext trust/update/list` | `src/mcode/cli/ext_command.{hxx,cxx}` (new) | ~300 |
| Unicode escape renderer | `src/mcode/support/unicode_scan.{hxx,cxx}` (new) | ~120 |

**≈ 940 new, ≈ 75 changed, zero new dependencies.** SHA-256 from the already-linked
OpenSSL; the atomic write through `workspace::write_file`.

**Seam:** everything attaches at the loader, ahead of `lua_host::create`. The only
thing crossing into the VM is bytes. No new registry, no new event, nothing in
the loop.

---

## Decision 2 — `mcode ext install` (Slice A, same subsystem)

The install **is** the grant, because the user typed the command — the position
`25` already takes.

```
mcode ext install git:owner/repo@<ref>
```

1. **Resolve before fetching.** `git ls-remote <url> <ref>`. An annotated tag
   emits **two** lines and only the `^{}` line's OID is the commit — a lockfile
   written from the first line records the tag object. Refuse if the ref matches
   nothing or matches more than one object; a tag that does not exist must not
   fall back to a branch of the same name.
2. **Clone to staging** with `--depth 1 --single-branch --no-tags --no-checkout`,
   `-c core.hooksPath=<empty>`, `-c protocol.file.allow=never`. No
   `--recurse-submodules`, and a repo containing `.gitmodules` is **refused with
   a message** — submodules are an execution path (CVE-2024-32002,
   CVE-2025-48384).
3. **Check out the resolved SHA detached**; assert `git rev-parse HEAD` equals it.
4. **Validate the manifest** with the *same* `load_manifest` the loader uses, so
   install-time and load-time acceptance cannot drift.
5. **Compute the tree digest** by the same code path as load time.
6. **Write the grant and the lockfile entry.**
7. **Report in one screen:** name, version, resolved commit, tree digest,
   declared permissions verbatim, suspicious-codepoint count.

**Lockfile:** `<workspace>/.mcode/extensions.lock` (project scope, committed) or
`<user-state>/mcode/extensions.lock`. TOML — this file is written for humans to
read and diff, and `support/toml.hxx` exists.

```toml
[[extension]]
name = "git"
source = "git:owner/repo"
ref = "v0.3.0"            # as written; informational only
commit = "a1b2c3d4..."    # the pin
commit_format = "sha1"    # git's SHA-1→SHA-256 transition makes length non-constant
tree_sha256 = "..."       # the digest the grant was written against
api_version = 1
permissions = ["fs_read", "spawn"]
```

`commit` **and** `tree_sha256` both, because a `.gitattributes` EOL filter or
`core.autocrlf` makes the working tree differ from the committed blob while the
commit ID stays valid — Neovim's Windows `:trust` bug in a different costume.

**Authority split:** the lockfile is the install-time record; the trust store is
the load-time authority. The loader reads the trust store and never shells out
to `git`.

**`ext update`** re-resolves, re-clones, computes the new digest, and shows a
diff of (version, commit, digest, permissions), requiring confirmation. A
**permission set that grew** is called out explicitly — an update that silently
adds `spawn` is the rug pull `12` already lists.

**Not in v1:** Sigstore/cosign/in-toto/SLSA. Keyless signing needs an OIDC
identity and a live Fulcio/Rekor round trip on a load path; there is **no
first-party C++ Sigstore client** (Go, Python, Rust, TS, Java only), so it means
a dependency or shelling out; and npm's own docs concede provenance "does not
guarantee the package has no malicious code." SLSA defends tampering *during and
after* a build, while mcode's threats assume the author is the attacker. Record
the hook instead: the lockfile already carries `commit` and `tree_sha256`, so an
optional attestation field is additive later.

**Cost:** ≈ 450 LOC for the CLI + dispatch, sharing all of Decision 1's machinery.

---

## Decision 3 — Network egress control (Slice B)

### Verified this session

| Finding | Evidence |
|---|---|
| `allow_network` is set in exactly one place | `tools/exec_tools.cxx:242` → `false` |
| Linux honours it | `sandbox_linux.cxx:117` (Landlock `handled_access_net`), `:235` seccomp fallback for ABI < 4 |
| macOS honours it | `sandbox_macos.cxx:64–67` — `(deny network*)` |
| **Windows does not** | `sandbox_windows.cxx` has no network handling at all |
| Asio + Beast are already in the tree | `net/http_client.cxx:5–12`; `Boost::process`, `Boost::boost` linked |
| The proxy needs no new dependency | Same Boost, same OpenSSL |

### Why both a proxy and a taint flag

The evidence forces a split, and the split is the design:

- **Every detection-, training-, or prompt-based defense is broken.** *The
  Attacker Moves Second* (`arXiv:2510.09023`) bypasses 12 published defenses at
  **>90% ASR** — Spotlighting and Prompt Sandwiching, reported at ~1%, reach
  **>95%**; the ProtectAI detector reaches **>90%**; human red-teaming **100%**.
- **One class survived, and the attackers say so explicitly.** They *skipped*
  plan-then-execute defenses including CaMeL because "the attack is guaranteed
  to fail … as the planning phase is done independently of the triggers." Their
  own caveat is equally load-bearing: it costs utility and applies only where a
  plan can be fixed in advance.
- **The proxy is the anti-exfiltration control; plan-then-execute is the
  anti-injection control.** Neither is sufficient.

### `egress_policy` — the decision object (no I/O, fully testable)

`src/mcode/net/egress_policy.{hxx,cxx}`. Attaches to the **registry**.

```cpp
enum class egress_decision { allow, deny, ask };

struct egress_rule {
	std::string host;          // exact, or one leading "*." — never a bare "*"
	std::uint16_t port = 443;  // no default-port wildcard
	bool allow = true;
};

class egress_policy {
public:
	[[nodiscard]] auto decide( std::string_view host, std::uint16_t port ) const
		-> egress_decision;
	[[nodiscard]] auto address_is_denied( const ip_range& address ) const -> bool;
private:
	std::vector< egress_rule > rules_;
	std::vector< ip_range > denied_resolved_;  // loopback, link-local, multicast,
	                                           // cloud metadata, own interfaces
};
```

Precedence: `deny` → `allow` → `strict_allowlist` → per-command → ask → deny.
**Default deny.** An empty allowlist means no network.

### `egress_proxy` — the listening process

Two loopback listeners on ephemeral ports: **HTTP `CONNECT`** (RFC 9110 §9.3.6)
for HTTPS and absolute-form HTTP, and **SOCKS5 no-auth** (RFC 1928) for
everything else TCP. Per connection: parse target → `decide` → **resolve once,
drop denied addresses, dial the survivor** (no second lookup, so a DNS rebind
cannot swap it) → relay. Refuse CONNECT targets outside a configured port set,
per RFC 9110's warning about tunnels to reserved ports. Both the connection map
and the pending-prompt queue are bounded.

The policy object is shared with the harness's own `net::http_client`, so the
in-process fetch tool cannot be a bypass — closing `12`'s residual risk #9
("host-side helpers crossing the boundary").

### Wiring: the OS rule is the boundary, the env var is the hint

`sandbox_profile` gains an `egress_endpoint { host, http_port, socks_port }`.
`apply_sandbox` sets `HTTP_PROXY`/`HTTPS_PROXY`/`ALL_PROXY`/`NO_PROXY` in
`minimal_environment()` (`proc/process.cxx:139`), which already scrubs the
environment to a known-safe set on all three platforms.

| Platform | Change | Honest capability |
|---|---|---|
| **Linux** | Landlock ABI ≥ 4 already exposes `LANDLOCK_ACCESS_NET_CONNECT_TCP` (`sandbox_linux.cxx:75`). Instead of denying all TCP, **deny all TCP except the proxy port**. Keep the seccomp fallback for ABI < 4, which can only deny | `enforced` at ABI ≥ 4, `deny_only` below |
| **macOS** | Replace `(allow network*)` with `(deny network*)` plus `(allow network-outbound (remote ip "localhost:PORT"))` per proxy port. **Never `(local ip …)`** — it is evaluated against `INADDR_ANY` at `connect()` time and admits everything | `enforced` |
| **Windows** | `FwpmEngineOpen0` needs `FWPM_ACTRL_OPEN` (admin), and the reference implementation needs a one-time elevated install. Keep the integrity-level write boundary, set the env vars, and **report `best_effort`**. Do not claim enforcement | `best_effort` |

Split `sandbox_network_support` further: `unavailable | best_effort | mediated |
enforced`, where `mediated` = proxy reachable and direct egress blocked, and
`enforced` additionally means an unprivileged child cannot bypass it. Windows
lands on `best_effort` in v1.

### The plan-then-execute half: a taint bit, not an interpreter

`src/mcode/perm/`. One boolean and one branch ahead of the rule merge, alongside
the existing plan-mode and hard-deny-floor checks:

- Set `untrusted_ingested = true` when a tool result from an untrusted source
  enters the context: web fetch/search results, MCP output below the trusted
  tier, cloned repo content, files outside the workspace's own source.
- While set, `net`-class actions and out-of-workspace writes resolve to `ask`
  **regardless of rules and regardless of `--yolo`**, exactly as plan mode and
  the hard-deny floor already do. The flag is **never cleared** by tool output;
  only a new session clears it.

This is Meta's Rule-of-Two `[B]`→`[C]` gate and CaMeL's control-flow guarantee,
implemented as a session bit rather than a 6,763-line Python interpreter. mcode
already has the structural half by accident: the permission engine decides on
**parsed argv and a canonicalized path, before the call runs** — not on model
output.

### What it does not stop

1. **Exfiltration through an allowlisted destination.** Allowing `github.com`
   permits a push to any repo the token reaches. An allowlist is not DLP.
2. **DNS-based exfiltration.** Resolution is not fenced on Windows or macOS.
3. **Timing and size side channels.** CaMeL's own §7 documents a private value
   leaked through loop count and read off a web server's access log — against a
   capability system strictly stronger than a taint bit.
4. **Text-to-text attacks.** A summary can lie.
5. **Credential *use*, not theft.** Already `12` residual risk #6.
6. **Domain fronting**, which defeats name-based allowlisting outright.
7. **Exfiltration via the model API** — by design.
8. **Anything outside the sandboxed child:** MCP servers, hooks, host-side tools.

**The lethal trifecta is not broken.** mcode still has all three. What the design
buys is that the agent cannot *choose* the destination and untrusted content
cannot *choose the plan*. That is a real reduction and it is not elimination.

### Cost

| Component | LOC |
|---|---|
| `egress_policy` (matcher, resolved-address deny set) | ~180 |
| `egress_proxy` (CONNECT relay, SOCKS5 handshake, resolve-then-dial, pumps, bounds) | ~450–600 |
| Env injection + per-platform rules | ~60 |
| Taint bit in `perm/` | ~80 |
| Probe-based tests | ~250 |

**≈ 1,000–1,200 LOC, zero new dependencies.** The proxy starts lazily on the
first sandboxed spawn, so **cold start is unaffected**; idle RSS grows by a few
hundred KB, inside the 30 MB ceiling.

---

## Decision 4 — The verification gate and the measurement spine (Slice C)

### Verified this session

| Finding | Evidence |
|---|---|
| The gate mechanism exists and is correct | `loop.cxx:431–468` runs a caller-set command and reads `/exit_code` |
| **Nothing can arm it** | `set_verification_command` (`loop.hxx:448`) has **zero callers** — every run takes `Verify --no_tests--> Handoff` |
| The command comes from configuration, not the model | Correct by construction (`11`) |
| The eval suite is deterministic and model-free | 13 builtin tasks in `eval/suite.cxx:110`; `pass_at_k`, `pass_power_k`, JSONL records |

### The gate

1. **Arm it from config and CLI:** `[verify] command = "..."` and
   `--verify "<cmd>"`. Empty stays a legitimate, reported state.
2. **Add the verdict vocabulary** `11` specifies and `eval/suite.hxx` lacks:
   `verified | claimed | failed | no_tests`. `claimed` is the model asserting
   success with no command configured or a command that did not run — the state
   that must never be reported as `verified`.
3. **Refuse self-certification of the gate.** A run whose verification command
   was modified by the agent during the run is reported as `claimed`, not
   `verified`. The gate's own command is not agent-writable state.
4. **Guard the classic escape.** Editing tests to make them pass is detected by
   hashing the files the verification command's *project* declares as tests
   before and after, when the project declares them. Where it does not, say so —
   the honest output is `verified (command exit 0; test-file integrity unknown)`.

### Why this is the trustworthiness measurement

The measured failure mode is large and the mechanism is understood:

| Study | Setting | Rate |
|---|---|---|
| *Confident and Wrong* (`arXiv:2603.25764`) | 1,750 trajectories, 50 SWE-bench Verified tasks, 4 models, 5 runs each | GPT-5 **submits on 100% of runs, resolves 44%**; silent semantic failure is **68%** of its failures |
| *From Confident Closing to Silent Failure* (`arXiv:2606.09863`) | 9,876 τ²-bench + 1,879 AppWorld trajectories | False success = **45–48%** of failures; **75.8%** of self-assessing AppWorld trajectories |
| *OverclaimBench* (`arXiv:2609.20812`) | 1,140 runs, 12 models | **67.9%** of runs fail to read every file they were asked to review; **80.4%** of those are misleading |
| *The Completion Illusion* | 896 micro-task instances, 4 models | **Every run self-reported a perfect score**; actual accuracy 85.7–95.5% |
| METR, *Recent Frontier Models Are Reward Hacking* | RE-Bench (n=128) vs HCAST (n=1087), o3 | **30.4% vs 0.7%** — >43× where the scoring function is visible; one task 21/21 |
| *How Coding Agents Fail Their Users* (`arXiv:2605.29442`) | 20,574 real sessions, 1,639 repos | Inaccurate self-reporting = **22.58%** of misalignment episodes, and its share grows over time |

Two structural findings matter more than the rates. **Reliability reads as
competence:** the per-task resolve distribution is sharply bimodal (5/5 or 0/5),
so consistency *amplifies* a committed misinterpretation — the agent re-derives
the same wrong fix. And **the same class already appears here:** `docs/41`
defect 6 recorded `bash` returning `ok: true` alongside `exit_code: -1073741502`
for a process that never started, and defect 4 a directory-scoped `grep`
returning a well-formed `count: 0` after skipping every file. Both turned a
failure into a plausible success.

### The measurement

Published as a number in the repo, reproducible from the fixture suite:

**`false_success_rate`** = runs reported `claimed` (or `verified` with an
unarmed gate) whose independent re-run of the verification command fails,
divided by all runs reported as success. Target **0**; the metric is the point,
not the target.

Supporting counters: `verified_share`, `no_tests_share`, `gate_disarmed_share`,
and **`artifact_runs`** — the published-artifact smoke run that caught the 0.0.2
packaging break, added to CI permanently.

**No test suite is reported as `no_tests`, never as verified.** Slow suites are
reported with their wall clock and a timeout, not silently skipped.

### Cost

≈ 120 LOC in `loop`/`config`/`cli`, ≈ 150 LOC in `eval/` for the counters, and
one new CI job. Zero new dependencies.

---

## Decision 5 — The context controller (Slice D)

### Verified this session

| Finding | Evidence |
|---|---|
| **The meter reports spend as occupancy** | `context_used()` → `budget_.tokens_used` (`loop.hxx:458`), compared against `context_capacity()` at `cli/repl_view.cxx:64`, `cli/slash.cxx:152,435`, `tui/render_view.cxx:132`. Will exceed 100% in any long session |
| **Compaction is truncation** | `maybe_compact` keeps the first 4 events and the newest tail, drops the middle, writes a `context.compaction` record. No summary, no re-read of modified files |
| **Four constants are dead** | `TOOL_CLEAR_TRIGGER_FRACTION`, `CLEAR_AT_LEAST_FRACTION`, `MAX_POST_COMPACT_REREADS`, `POST_COMPACT_REREAD_TOKENS` — `loop.hxx:68–74`, read nowhere |

### The mechanism

One type, `context_controller`, owned by `agent_loop` as a member. Consulted at
exactly two points and otherwise inert: `plan( history, occupancy, turn_boundary )`
before each request, and `observe( usage )` after each response. The no-op path
allocates nothing.

**A three-rung ladder, evaluated pre-request:**

| Rung | Condition | Model call? |
|---|---|---|
| **1. Clear** | `prompt_tokens ≥ 0.60 × usable_window` **and** the reclaimable span ≥ `0.20 × usable_window` | no |
| **2. Fold** | `prompt_tokens ≥ 0.80 × usable_window` **and** a safe point (no pending tool call, last step resolved, or between user turns) | yes |
| **3. Rewrite the pinned block** | only as part of 1 or 2, never standalone | no |

Rung 1's gate is not taste — it is cache arithmetic. With read = 0.1× and
write = 1.25×, converting a region from cached-read to cache-write costs 1.15×
once and saves 0.9× per later turn; break-even is ≈1.28 turns. Below 20% of the
window the payback usually does not land. **Cut deep and cut once.**

Rung 2's safe point is a rubric the *harness* owns, not the model. SelfCompact
(`arXiv:2606.23525`) found the compaction tool alone is "unevenly used … often
invoked at unhelpful moments or not at all," and the rubric alone cannot act —
both are needed. That is an argument for the harness owning the trigger and the
human owning the command.

**Four prompt regions; the controller only touches the last two:**

| Region | Content | Stability |
|---|---|---|
| **R0** | tools array, then system prompt | frozen for the session |
| **R1** | **pinned state block**, regenerated from session state — not from the summary | byte-stable between rewrites |
| **R2** | rolling summary, one message replaced in place | replaced atomically |
| **R3** | tail: ≤3 turns / ≤20K tokens, aligned to a tool-pair seam | append-only |

**R1 is the centre of gravity and it is not the summary.** It carries the
user's original task verbatim, the current plan, modified files with their
last-read content hash, unresolved errors with their failure class, and
restorable stubs for dropped tool results. It is rebuilt from deterministic
state, so it changes only when something real changed — which is what makes the
prefix cache-stable, and it keeps the load-bearing facts at the **front** of the
prompt, which is the position Lost in the Middle (`arXiv:2307.03172`) and
Attention Basin (`arXiv:2508.05128`) say works. A summary inserted at the cut
point lands in the middle, which is neither of the two positions that work.

**Clearing is the rung that does most of the work.** JetBrains' *The Complexity
Trap* (`arXiv:2508.21433`) measured that **84%** of a turn's tokens are
environment observations, and that observation masking **more than halves cost**
while matching LLM summarization's solve rate (Qwen3-Coder-480B: raw 53.8% →
masking **54.8%**, 52% cheaper). Summarization is not automatically the better
lever for being the more sophisticated one.

**The summarizer runs on the same model as the session** — a deliberate reversal
of OpenHands. A different model has a different cache, so the fold would
re-prefill the entire history at 1.0× instead of reading it at 0.1×: a 10× swing
on the session's largest request, to save on output tokens.

### What the user sees

Three numbers from data already logged (`loop_dispatch.cxx` writes per-request
`model.usage`): **fill** (`(input + cached_read + cache_write)` of the *last*
request ÷ usable window), **cache hit ratio**, and **spend**. The status-line
meter reads *fill*, not spend. `/context` becomes a breakdown (`system | tools |
pinned | summary | tail | tool results | free`), plus `epochs: N · last rewrite
4 turns ago · 3.1k tokens re-cached` so a rewrite is visible as a decision, and a
`~N turns to fold` projection **omitted below three samples**.

### Cost

| Item | LOC |
|---|---|
| `agent/context_controller.{hxx,cxx}` (new) | ~350 |
| `loop_context.cxx` — `maybe_compact` replaced | −80 / +40 |
| `loop.hxx` — constants, occupancy accessor, fix `context_used()` semantics | +20 |
| `loop_dispatch.cxx` — feed `observe(usage)`, add `epoch` | +15 |
| `slash.cxx` — real `/compact [focus]`, `/context` breakdown | +40 |
| `render.cxx` / `render_view.cxx` — meter reads fill | +25 |
| `docs/05` rewrite (the trigger table is wrong in three places) | — |

**≈ +490 / −80, one new file, zero new dependencies.** It must stay in its own
file — the ≤600-line cap means it cannot fold into `loop_context.cxx`, which is
the right shape anyway: the loop asks a question, the controller answers it.

### What is measurable

Cache hit ratio and its recovery after a rewrite (falsifies the 20% threshold);
rewrite amortization vs the predicted ≈1.28 turns; fill trajectory; **post-fold
recall** — the one metric that measures summarization loss rather than cost;
**evidence retention** — `errors_dropped_without_class`, target **zero**, a hard
invariant not a metric; and projection error (±2 turns or the line is deleted).

---

## Decision 6 — MCP over Streamable HTTP (Slice E)

`07` already specifies this; only the implementation is missing.

**Minimal profile:** POST-only client, accept both `application/json` and
`text/event-stream` responses, honour `MCP-Session-Id` and `MCP-Protocol-Version`
headers, DELETE on shutdown, 404 → re-initialize. **Skip the GET listening
stream** — it exists for server-initiated requests, and if the client does not
advertise sampling/elicitation, servers will not use it. Legacy HTTP+SSE is a
~15-line compatibility probe: on POST failure with 400/404/405, GET the URL,
expect an `endpoint` event, route POSTs there.

**Target `2025-06-18` core.** Do not chase `2026-07-28` — the installed server
fleet overwhelmingly speaks 2024-11-05…2025-06-18, and the deprecation policy
gives a 12-month window.

**The reconnect lesson is load-bearing.** `[CC]`'s stdio client treated servers
as permanently failed after a disconnect while HTTP got exponential backoff, and
a follow-up bug deregistered respawned servers' tools. stdio respawn →
re-initialize → re-list must be one automatic path here, not an afterthought.

**New files:** `mcp/transport_http.{hxx,cxx}` behind the existing `transport.hxx`
interface. **No new dependency:** Boost.Beast already does HTTP and SSE-shaped
streaming in `net/http_client.cxx`, and `net/sse.cxx` already parses event
streams. **≈ 300 LOC.** MCP is a `ToolSource`, so nothing in `agent/` includes an
MCP header and the loop is untouched.

---

## Decision 7 — Memory: no automatic memory (Slice F)

### The evidence changed the answer

`09` designs an always-injected `MEMORY.md` index plus an SQLite FTS5 store with
an LLM write pass. The measurements do not support it:

| Finding | Number |
|---|---|
| Memory systems under a matched toggle (VibeMemBench, `arXiv:2609.23570`; 111 targets, 3,634 trajectories, scored by the repo's own tests) | Failed to beat memory-off in **11 of 12** solver×system pairings; no interval above zero |
| The one positive result | A **frozen, execution-verified** 4-field record, +1.1…+4.5 pp on 4 of 5 solvers — **all five intervals cross zero** |
| The irrelevant-memory control | **0.7–3.4 pp below** the memory-off baseline — injected text alone reproduces none of the gain |
| Failure attribution (231 pairings) | **Form degradation 69.3%**, coverage 16.0%, use 13.4%, ranking **1.3%**. Records averaged 65 lines vs the 4-field record |
| Strip ablation | The harm tracks **transcript volume**, not instruction semantics |
| Poisoning (`arXiv:2606.04329`) | 4 write channels, 9 vulnerabilities; **always-injected** memory RSR **64.70%** vs **17.40%** for tool-invoked retrieval |
| Staleness (`arXiv:2605.06527`) | New evidence retrieved in **77.5%** of cases, old evidence in the top-3 in **60.5%**, yet only **3.3%** of old entries were judged to need an update |
| Cost (`arXiv:2605.18854`) | No condenser significantly alters hypothesis quality; LLM condensers **+24–94% tokens**; masking tool output **−8.6%** |
| `[CC]`'s auto-memory load | 200 lines / 25 KB ≈ **6.2K tokens** — ~73% of mcode's entire 8.5K session-start budget |

### The design

**A user-authored, agent-read, single-file ledger.** One file:
`<user-config>/mcode/projects/<repo-id>/notes.md`. Markdown, append-mostly,
one `## YYYY-MM-DD — <source>` heading per entry where source is `user`,
`correction`, or `observation`. No index, no topic files, no SQLite, no vectors,
no graph, no decay formula, no `supersedes` links, no `gc` command.

**Write policy — a human writes, or nothing is written:**
1. The user edits the file.
2. `/remember <text>` appends the user's text **verbatim**. No model in the loop.
3. The agent may *offer* a line; it has **no write capability to the ledger**.

**Never write from tool output, web content, MCP responses, repo contents, or a
session summary.** Those are the four measured write channels. **Never write a
command** — commands belong in `AGENTS.md`, where CI keeps them true; this
removes the most damaging stale class and is why no decay machinery is needed.
**Never write during a session.**

**Retrieval:** one ~25-token pointer line in the prompt-assembly function next to
the existing instruction-chain and skill-index blocks, emitted only when the file
exists and is non-empty, suppressed when the extension exposing `memory_search`
is disabled. Plus a `memory_search` Lua extension that greps the ledger and
returns entries verbatim, capped by the existing search cap — structurally
identical to `extensions/skills/init.luau`, taking a **query string, never a
path**. Plus the existing `read`/`grep` over the known path.

Letta's counter-benchmark is the direct precedent: a file plus `grep`/`open`
scored **74.0%** on LoCoMo, above mem0's reported 68.5% for its graph variant.

**Cap: 200 lines, hard, enforced at write time.** Past it, `/remember` refuses
naming the oldest entries, and the user prunes.

**Cost: ≈ 280 LOC, one extension directory, zero new dependencies.** Against
`09`'s ~1,500–2,500 LOC and a 10th direct dependency — the repo sits at 9 against
a budget of ≤12 with size binding, and SQLite would be the largest single
addition.

**Measurement gate for anything more:** a deterministic writer recording an
observed fail→pass pair from the harness's own verification gate. It is an
*event*, not an inference, so it dodges the form-degradation mechanism. Not in
v1 because nobody has measured that a single-session fail→pass record changes
behaviour.

---

## Decision 8 — Read-only subagents (Slice G)

`docs/10` now carries the full design; this records the decision and the
sequencing.

**Ship `explore` / `plan` / `review`, read-only, in v1. Do not ship parallel
writers.** Every published multi-agent *win* is research-shaped or has a perfect
verifier: Anthropic's +90.2% is breadth-first research at ~15× chat tokens, and
Carlini's compiler had a near-perfect test oracle and was file-disjoint by
construction. The coding counter-evidence is stronger — MAST
(`arXiv:2503.13657`, 150+ traces, 5 frameworks) finds "the gain … remains
minimal compared to single agent frameworks" and ChatDev correctness "as low as
25%"; Agentless (`arXiv:2407.01489`) deleted the agent loop and scored **32.00%**
on SWE-bench Lite at **$0.70**, best score and lowest cost among the open agents
it compared.

**Read-only is a capability set, not a prompt.** The child's dispatcher resolves
its tools from the same registry with only `read`/`glob`/`grep` present, so a
write call fails schema validation rather than reaching an approval prompt. **No
shell in a read-only child** — Cline's Plan mode is the cautionary case, where
`python -c` still writes. Depth 1, no nesting, no approval inheritance, and a
`--no-subagents` kill switch.

**Context boundary:** a fresh window, not a fork. The parent must write
constraints into the payload — that forced explicitness is what kills Cognition's
"implicit decisions" failure, and it makes the child dumber about the task by
design, so it only pays when the work is self-contained.

**Return contract:** `{summary ≤2K tokens, findings[{claim,file,line}],
artifacts[], open_questions[], tokens_used}` — summary **plus paths, never raw**.

**Budget:** child usage folds into the run budget (one accounting). ≤4 concurrent,
≤10 total per run without consent; breach cancels **and harvests** partial output.

**Sequenced last because fan-out multiplies an unbounded-execution problem.**
Subagents before the trust gate would run N copies of code the user never
approved. The one open question is recorded in `10`: whether read-only fan-out
earns its ~15× token multiplier on coding repos is unmeasured.

---

## Sequencing, and why

| # | Slice | Why here |
|---|---|---|
| 1 | **A — Trust gate + `ext install`** | Cheapest. Makes `12` true and closes a proven arbitrary-execution path. The install path is the same subsystem, so it lands together |
| 2 | **B — Egress** | Highest value, hardest. `12` calls it the control that matters; Windows cannot be claimed as enforced and the doc must say so |
| 3 | **C — Verification + spine** | Makes every claim in 1 and 2 provable, and adds the artifact run that caught the 0.0.2 break |
| 4 | **D — Context controller** | Fixes a user-visible bug and the doc that misdescribes it. Independent of 1–3 |
| 5 | **E — MCP HTTP** | Days, removes a hard capability ceiling, no new dependency |
| 6 | **F — Memory** | Small and honest. A *reduction* from `09` |
| 7 | **G — Subagents** | Last on purpose: fan-out multiplies whatever the trust gate has not yet bounded |

A, B and D are independent and can proceed in parallel; C should follow A and B
because it measures them.

## The measurement spine

| Metric | Target | Why |
|---|---|---|
| Extension loaded with no grant | **0** | The defect this batch exists to fix |
| TOCTOU window between hash and execute | **0 bytes re-read** | The snapshot makes it structural, not narrow |
| `false_success_rate` | **0** | The published trustworthiness number |
| Verification gate armed in CI | **every run** | Currently zero callers |
| Egress: direct connection from a sandboxed child | **refused** on Linux/macOS; **unmediated, reported** on Windows | `16` M1 promised this and Windows never got it |
| Proxy bypass via env unset | **fails** — the OS rule is the boundary | The env var is a hint |
| `errors_dropped_without_class` | **0** | Hard invariant: a fold must not delete unrepresented failure |
| Context meter vs true occupancy | within 5% | Currently reports cumulative spend |
| Cache hit ratio recovery after a rewrite | ≤2 turns | Falsifies the 20% clear threshold |
| `artifact_runs` in CI | **1 per release** | Caught the 0.0.2 packaging break |
| Cold start / idle RSS / binary size | ≤15 ms / ≤30 MB / ≤25 MB | `01` budgets; unchanged, and all three slices add no dependency |

## Acceptance

1. A cloned repo containing `.mcode/extensions/backdoor/init.luau` loads it **inert**, reports it, and runs the session.
2. A granted extension whose bytes change becomes inert and is reported; the grant is **not** rewritten.
3. `mcode ext install git:owner/repo@v1` resolves the peeled commit, refuses `.gitmodules`, and writes both the grant and the lockfile.
4. A sandboxed child cannot reach a host not on the allowlist, on Linux and macOS by the OS rule; Windows reports `best_effort` rather than claiming parity.
5. With `[verify] command` set, a run whose command fails is reported `failed`, never `verified`; with no command it is `no_tests`, never `verified`.
6. `false_success_rate` is computed and printed by the eval suite.
7. The context meter reads *fill*, and `/context` shows the region breakdown.
8. `/compact` refuses with a named reason while a tool call is pending.
9. The published artifact is downloaded and smoke-tested in CI.
10. `_clgate.py` exit 0; `mcode_tests` green; `--smoke` 0 failures; all three platforms tier-1.

## Sources

Retrieved this session. Vendor claims labelled.

**Prompt injection and egress**
- https://simonwillison.net/2025/Jun/16/the-lethal-trifecta/ — the trifecta framing
- https://ai.meta.com/blog/practical-ai-agent-security/ — Agents Rule of Two
- https://arxiv.org/abs/2503.18813 — CaMeL: 77% solved vs 84% undefended, 949 attacks, 6,763 LOC interpreter, §7 side channels, §9.3 "not fully solved"
- https://arxiv.org/abs/2506.08837 — design patterns; §4.10 the software-engineering-agent case study
- https://arxiv.org/abs/2510.09023 — *The Attacker Moves Second*: >90% ASR against 12 defenses; the plan-then-execute skip
- https://arxiv.org/abs/2406.13352 — AgentDojo: 629 security cases, 57.69% baseline ASR
- https://raw.githubusercontent.com/anthropics/sandbox-runtime/main/README.md — the proxy pattern, its documented limits, the macOS `local ip` pitfall
- https://www.rfc-editor.org/rfc/rfc9110 — CONNECT §9.3.6; https://www.rfc-editor.org/rfc/rfc1928.txt — SOCKS5
- https://raw.githubusercontent.com/torvalds/linux/master/Documentation/userspace-api/landlock.rst — the ABI ladder, network rules from ABI 4
- https://learn.microsoft.com/en-us/windows/win32/fwp/application-layer-enforcement--ale- and `.../nf-fwpmu-fwpmengineopen0` — WFP ALE layers; `FWPM_ACTRL_OPEN`
- https://developers.openai.com/codex/config-reference.md and https://learn.chatgpt.com/docs/sandboxing.md — `sandbox_mode`, network off by default `[VENDOR]`
- https://docs.openhands.dev/openhands/usage/architecture/runtime — Docker by default `[VENDOR]`

**Extension trust and supply chain**
- https://raw.githubusercontent.com/neovim/neovim/master/runtime/lua/vim/secure.lua and `.../runtime/doc/editing.txt` — the trust DB, the `:trust [file]` TOCTOU warning, the directory-by-name weakness
- https://api.github.com/repos/neovim/neovim/issues/35069 — the `&allow` removal
- https://www.truesec.com/hub/blog/glassworm-self-propagating-vscode-extension — 7 extensions, 35,800 downloads, variation-selector payloads
- https://www.aikido.dev/blog/glassworm-returns-unicode-attack-github-npm-vscode — 151+ bespoke commits
- https://arxiv.org/abs/2111.00169 — Trojan Source
- https://raw.githubusercontent.com/luau-lang/luau/master/Ast/src/Lexer.cpp — strings and comments accept any UTF-8; identifiers do not
- https://cwe.mitre.org/data/definitions/367.html — TOCTOU mitigations
- https://docs.deno.com/runtime/fundamentals/security/ — deny by default, `--allow-run`/`--allow-ffi` as full escapes, same-thread same-privilege
- https://code.visualstudio.com/docs/editing/workspaces/workspace-trust — "can't prevent a malicious extension" `[VENDOR]`
- https://slsa.dev/spec/v1.0/threats — source threats out of scope; https://docs.npmjs.com/generating-provenance-statements — "does not guarantee" `[VENDOR]`
- https://raw.githubusercontent.com/git/git/master/Documentation/git-ls-remote.adoc — the `^{}` peeled-tag line

**Verification**
- https://arxiv.org/abs/2603.25764 — Confident and Wrong
- https://arxiv.org/abs/2606.09863 — From Confident Closing to Silent Failure
- https://arxiv.org/abs/2609.20812 — OverclaimBench
- https://arxiv.org/abs/2605.29442 — How Coding Agents Fail Their Users
- METR, *Recent Frontier Models Are Reward Hacking* — RE-Bench 30.4% vs HCAST 0.7%
- `docs/41` defects 4 and 6 — the two measured false-success cases in this repo

**Context**
- https://research.trychroma.com/context-rot — 18 models, 194,480 calls, distractors compound
- https://arxiv.org/abs/2307.03172 — Lost in the Middle; https://arxiv.org/abs/2508.05128 — Attention Basin
- https://arxiv.org/abs/2508.21433 — The Complexity Trap: 84% observation share, >2× cost reduction, 53.8%→54.8%
- https://arxiv.org/abs/2606.23525 — SelfCompact: 30–70% lower token cost, the rubric is load-bearing
- https://arxiv.org/abs/2510.11967 — Context-Folding: 10× smaller active context; https://arxiv.org/abs/2510.24699 — AgentFold
- https://arxiv.org/abs/2601.06007 — Don't Break the Cache: 41–80% cost, 13–31% TTFT
- https://manus.im/blog/Context-Engineering-for-AI-Agents-Lessons-from-Building-Manus — prefix stability `[VENDOR]`
- https://arxiv.org/abs/2404.06654 — RULER; https://arxiv.org/abs/2502.05167 — NoLiMa

**Memory**
- https://arxiv.org/abs/2609.23570 — VibeMemBench: 11/12 pairings ≤ memory-off, form degradation 69.3%
- https://arxiv.org/abs/2606.04329 — memory poisoning: always-injected RSR 64.70% vs 17.40%
- https://arxiv.org/abs/2605.06527 — STALE: 77.5% retrieval, 3.3% update rate
- https://arxiv.org/abs/2605.18854 — condensers +24–94% tokens, no quality change
- https://arxiv.org/abs/2504.19413 — mem0: 66.88% vs full-context 72.90%
- https://www.letta.com/blog/benchmarking-ai-agent-memory/ — filesystem + grep 74.0% on LoCoMo
- https://docs.anthropic.com/en/docs/claude-code/memory — 200-line/25 KB auto-memory load `[VENDOR]`

**Subagents**
- https://cognition.com/blog/dont-build-multi-agents — the counter-position
- https://arxiv.org/abs/2503.13657 — MAST: "gains remain minimal", ChatDev as low as 25%
- https://arxiv.org/abs/2407.01489 — Agentless: 32.00% SWE-bench Lite at $0.70
- https://arxiv.org/abs/2405.15793 — SWE-agent ACI

**Repo**
- `docs/01-north-star.md`, `02-prior-art.md`, `03-architecture.md`, `05-context-engineering.md`, `06-tools.md`, `07-mcp.md`, `09-memory.md`, `10-subagents.md`, `11-reliability-and-evals.md`, `12-security.md`, `14-cpp23-stack.md`, `16-roadmap.md`, `25-distribution.md`, `26-first-batch.md`, `32-second-batch.md`, `36-third-batch.md`, `39-workstream-sandbox.md`, `40-non-obvious-constraints.md`, `41-*`, `42-agents-md-research.md`
