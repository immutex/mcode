# Distribution, Registry & Docs

> TL;DR: The extension site is an **index, not a registry** — no artifact hosting, no accounts; install coordinates point at git and a lockfile pins the SHA. Trust comes from the runtime hash-pinned grant (`12`), not from review, because review-gating measurably fails. Docs are MkDocs Material with the Lua reference **generated from LuaLS annotations** so types, editor intellisense, and the website share one source.

## The index, not a registry

pi.dev/packages is the cited model, and its defining choice is that **it is an index over someone else's registry**: artifacts live on npm, the gallery lists derived metadata, publishing is "publish an ordinary npm package", and discovery is a keyword gate. There is no account, no upload, no review, no hosting.

mcode copies that shape, with git as the substrate:

| Concern | Decision |
|---|---|
| Artifact hosting | **None.** Extensions are git repositories |
| Publishing | Push a repo with a valid `ext.toml`. Nothing to upload |
| Discovery | GitHub topic `mcode-extension` (or a `[registry]` block) gates ingestion into the index |
| Accounts | None. No login, no publisher identity to squat |
| Index | A generated JSON document rendered as static pages |
| Moderation | A pre-filled report link on every listing; delisting is an index action |
| Install | `mcode ext install git:owner/repo@<ref>` → clone into the **user scope** (`<user-config>/mcode/extensions/`) → record the resolved SHA in a lockfile. `<ref>` is any git revision (tag, branch, SHA); the lockfile stores the **resolved commit SHA**, so the format is ref-agnostic by construction |

The index can list an attacker's extension without that extension gaining anything, because **listing grants no trust**. That property is what makes an open index safe to run.

**Installing, by contrast, *is* a trust grant** — and that is deliberate, not an accident of the flow. `mcode ext install` writes to the **user scope**, which `19` treats as trusted-by-location, and it records the installed commit SHA. So the grant is explicit (the user typed the command), scoped (one extension), and hash-pinned (a later change to that extension invalidates it). The distinction the model rests on is: **a listing is a stranger's advertisement; an install is you deciding to trust it.** Project-scope extensions remain inert until a separate grant, because those arrive without the user typing anything (`12` T3).

Listing fields, kept deliberately small: name, one-line description, author, resource type, last-published age, install line, and links to source + report. Download counts only if we can compute them honestly from the git host — a fabricated popularity signal is worse than none.

## Trust model

**Open publishing + automated screening + explicit runtime trust. Not review-gating.**

Review gating was tested by the ecosystem and lost. Zed's review queue is documented at "a few weeks to one or two months" and closes PRs after 3 weeks of author silence. GlassWorm compromised 7 Open VSX extensions and one Marketplace extension totalling **35,800 downloads** by hiding payloads in **invisible Unicode characters** — a technique that defeats human review by construction. Obsidian's pre-2026 model reviewed only the *initial* submission and admits updates were never re-reviewed; their 2026 relaunch replaced review with **automated per-version scanning**, clearing 2,300 queued submissions in days.

The boundary that actually works is the one already in `12`: **hash-pinned, explicit, per-file trust grants, with project extensions inert until granted.** The registry is untrusted content by definition. Its job is discoverability, revocation, and signals — not safety.

| Layer | Mechanism |
|---|---|
| Ingestion | Automated manifest validation + permission-block sanity + malware-pattern scan (Obsidian's model) |
| Listing | Report link per extension; emergency delisting with an IoC note |
| Install | Pin to an immutable coordinate (git tag → **commit SHA in the lockfile**) |
| Execution | Hash-pinned trust grant (`12`, `19`) — unchanged by anything the registry says |
| Signals | Curated badge, scan status, last-published age. **Never** a safety claim |

Installing an extension means running its author's code with your privileges. The gallery says so, in one line, on every listing. pi does exactly this and it is the honest posture.

**Lua has no signing ecosystem to inherit.** LuaRocks' 2019 incident admitted "LuaRocks packages currently don't have signing and verification". There is nothing to build on, so v1 ships the minimum integrity story: git SHAs in a lockfile, plus optional `SHA256SUMS` for release tarballs. Attestation is a later, additive step.

## Manifest

The `[registry]` block extends `ext.toml` (`19`). **The loader ignores it entirely** — only the index consumes it — so it can evolve without any loader compatibility cost.

```toml
name = "git"
version = "0.3.0"            # semver, enforced by the index
api_version = 1                # integer floor, never a range (`19`)
permissions = ["fs_read", "spawn"]

[registry]                    # index-only; the loader never reads this
description = "Git-aware tools and commit conventions"
repository = "https://github.com/me/mcode-git"
license = "MIT"               # SPDX expression
keywords = ["git", "commit"]  # ≤10, free-form, discovery
categories = ["vcs"]          # fixed taxonomy
```

TOML throughout, consistent with config (`22`). A single format for config, manifests, and registry metadata means one parser, one set of conventions, and no JSON-vs-TOML translation layer.

## Versioning and revocation

| Rule | Rationale |
|---|---|
| **Published versions are immutable** | crates.io's best idea: reproducibility and rollback without deletion |
| **Yank, don't delete** | A yanked version cannot gain new dependents; existing lockfiles keep working |
| **Deprecate as data** | A `deprecated = {reason, successor}` flag surfaces on install and in the listing (npm's model) |
| **`api_version` is an integer, additive-only** | Immune to range-syntax bugs; pairs with `mcode.capabilities` for feature detection (`18`) |
| **Delisting is not uninstalling** | Removal is an index action; installed copies keep working |

`api_version` semantics, the extension directory layout, and the lockfile format are the **irreversible** choices — every published extension bakes them in. Categories, keywords, the index implementation, and the docs generator are all reversible.

## Update UX

**Never auto-update.** Auto-update of executable packages is repeatedly implicated in real incidents — GlassWorm's spread leaned on it, and mpv's docs warn about it explicitly.

```
$ mcode ext update
  git      0.3.0 → 0.4.0   (a1b2c3d → e4f5g6h)
  web      0.1.2 → 0.1.2   (unchanged)
  Apply? [y/N]
```

Show the version and commit diff, link the changelog, require confirmation. Neovim's `vim.pack` update flow — which opens a confirmation buffer you inspect before writing — is the best CLI-adjacent precedent found. Updates are an explicit command, never a background behavior.

## Docs site

**MkDocs Material**, matching WezTerm's precedent (the closest shape: config language + API + CLI in one nav) and the repo's Markdown house style. One build produces guides, the Lua API reference, and the extension gallery.

The Lua reference is **generated from LuaLS/LuaCATS annotations** via `lua-language-server --doc`, which emits `doc.json`:

- **One source of truth.** The same annotations give extension authors editor intellisense and type-checking, and give the site its reference pages. Annotations are not documentation overhead; they are the type layer.
- **Machine-readable.** `doc.json` renders into any generator and can power hover text, so the generator choice stays reversible.
- Neovim's `:help lua-plugin` explicitly recommends LuaCATS annotations for plugin authors and annotates its own codebase — the same play.

Publish **`.md` mirrors plus `llms.txt`** (Zed and pi both do this). Agent-readable docs are now table stakes, and it costs one build step.

Site sections: Getting started · Configuration (TOML reference) · Extension authoring · Lua API reference (generated) · Extension gallery (generated from the index) · CLI reference.

## Packaging and platform

Per-platform artifacts and signing are specified in `24`:

| Platform | Artifact | Signing |
|---|---|---|
| Windows | static `.exe` | Authenticode OV; SmartScreen reputation ramps |
| Linux | static musl tarball (x86_64, aarch64) | none needed |
| macOS | arm64 tarball | ad-hoc `codesign` minimum; Developer ID + notarize if distributed as a Homebrew cask |

Distribution channels: GitHub Releases first; Homebrew formula (not cask — no notarization required), winget manifest, and a scoop bucket. `curl | sh` and a tarball remain the universal fallback.

### The install scripts

Two scripts in the repository root, both served straight from `master`:

```
Windows   irm https://raw.githubusercontent.com/immutex/mcode/master/install.ps1 | iex
Linux     curl -fsSL https://raw.githubusercontent.com/immutex/mcode/master/install.sh | sh
macOS     curl -fsSL https://raw.githubusercontent.com/immutex/mcode/master/install.sh | sh
```

They download a release archive, **verify its SHA-256 against the release's
`SHA256SUMS`, and install** — in that order, and a missing checksum file is a hard
failure rather than a skipped check, because an unverified binary is the one thing
the step exists to prevent. Then they add the install directory to the user PATH
and hand off to `mcode setup`.

Design constraints, each of which the script has to respect:

| Constraint | Why |
|---|---|
| **POSIX `sh`, not bash** | `curl \| sh` runs under whatever `/bin/sh` is: dash on Debian and Ubuntu. No arrays, no `[[ ]]`, no `local` in the outer scope |
| **No elevation** | `/usr/local/bin` when it is already writable, `${HOME}/.local/bin` otherwise; Windows uses `%LOCALAPPDATA%` and the user-scope PATH |
| **Colour is opt-out and auto-off** | `NO_COLOR`, plus a non-tty stdout, so a piped log gets clean text |
| **Idempotent** | Re-running installs the new binary and never appends a second PATH entry or a second `[model]` section |
| **Predictable artifact names** | `mcode-<version>-<platform>.<ext>` is a contract between the packaging job and the scripts |

### `mcode setup`

The interactive part is a **subcommand of the binary**, not the installer script.
A wizard written in `sh` and another in PowerShell would drift, and the packaged
build would be the one that diverges.

```
$ mcode setup
  mcode setup
  Choose a provider. Writes one file: ~/.config/mcode/config.toml

  Provider
   > [OI]                           platform.openai.com
     Anthropic                      console.anthropic.com
     [OI]-compatible endpoint       any /v1/chat/completions gateway

  Model
   > gpt-5                          recommended
     gpt-5-mini
```

Arrow keys move, a digit jumps, Enter accepts. Colours resolve through the shipped
`theme_table`, so the wizard cannot drift from the TUI palette, and it degrades to
plain text on a 16-colour terminal and on `NO_COLOR`.

**Verification runs a real turn.** After writing the config, the wizard spawns its
own binary as `mcode exec --json "Reply with the single word: ready"` with the key
in the child's environment. A hand-rolled HTTP request would prove the endpoint
answers but not that the descriptor, credential and streaming parser agree; the
real path proves the thing the user is about to run. The key never reaches disk and
never appears in the process arguments. On failure the wizard offers to undo, and
restores the previous file byte for byte.

**The config is edited as text, not parsed and re-serialised.** There is no TOML
writer in the tree, and a round-trip would drop the `[models."…"]` pricing blocks
and the comments that carry their rationale. The `[model]` section is replaced in
place and everything else is preserved. The edit is **line-based** rather than
offset-based: a config edited on Windows carries CRLF, and a check for a bare `\n`
after the header misses it and appends a *second* `[model]` — which the loader
rejects as a duplicate on the user's next start rather than at setup time. That was
observed during development, not theorised.

`--provider`, `--model`, `--base-url`, `--api-key-env`, `--api-key`, `--no-verify`
and `--yes` take the scripted path, so CI and provisioning never wait on a prompt.
No terminal at all is the same path rather than an error.

### Licence obligations

mcode is Apache-2.0. The one embedded component with an attribution request is **Luau** (MIT, plus a request that user-facing documentation credit the language and link to <https://luau.org/>). Satisfied by the Licence section of `README.md` and `THIRD-PARTY-NOTICES.md`; the full texts also ship inside the Conan package under `licenses/`.

Every release artifact must carry both files. Conan packages resolve their own licences; only the vendored-in-binary components need this treatment, and today that is Luau alone. A new vendored dependency with an attribution clause is a release-checklist item, not a docs-afterthought.

## Traps

- **A wizard written twice.** An `sh` wizard and a PowerShell wizard drift, and the packaged build is the one that diverges. The interactive part is a subcommand of the binary; the scripts only fetch, verify and hand off.
- **Installing without verifying.** A missing `SHA256SUMS` is a hard failure, never a skipped check. An unverified binary is exactly what the step exists to prevent.
- **A CRLF config and an offset-based section edit.** The header check misses, a second `[model]` is appended, and the loader rejects the duplicate on the user's *next* start. Line-based, and covered by a test.
- **Re-serialising the config.** There is no TOML writer, and a round-trip would drop the `[models."…"]` pricing blocks and their rationale. Edit the section as text.
- **`bash` in a `curl | sh` script.** Debian and Ubuntu run it under dash.
- **Building artifact hosting before there is demand.** The index model is strictly cheaper and the ecosystem has not needed hosting yet.
- **Review-gating.** Weeks of queue latency, defeated by invisible Unicode. Automate screening; keep the runtime gate authoritative.
- **Auto-update.** Repeatedly implicated in real compromises. Explicit command, diff shown, confirmation required.
- **Treating the curated badge as a safety claim.** It is curation, not an audit.
- **Fabricating popularity signals.** A download count we cannot compute honestly is worse than no count.
- **Assuming a signature means safety.** npm's own docs concede provenance "does not guarantee the package has no malicious code".
- **Ignoring config/data channels.** Obsidian's worst 2026 incident abused *legitimate* plugins through a synced config vault — registry review of code does not cover data paths. mcode's rule: nothing in config executes.
- **Getting `api_version` or the layout wrong.** Irreversible once extensions exist.
- **A single format for docs and a different one for config.** TOML everywhere.

## Open questions

- Does the index get curated badges at v1, or is that a later policy layer? The roadmap currently defers it.
- GitHub topic vs an in-repo `[registry]` block as the discovery gate: the topic is zero-effort for authors but invisible outside GitHub; the block is explicit but requires the manifest to be read.
- Should `mcode ext update` be able to update a single extension by name, or always reconcile everything? Per-extension is finer but multiplies the confirmation prompts.
- Do we ship the `SHA256SUMS` path in v1, or is a lockfile SHA sufficient until someone asks?
- Does the gallery need a "verified author" concept, or does that imply a safety guarantee we cannot back?

## Sources

- https://pi.dev/packages — index model, listing fields, type/sort/name filters, install lines, report links
- https://raw.githubusercontent.com/badlogic/pi-mono/main/packages/coding-agent/docs/packages.md — `pi-package` keyword gate, manifest, pinning, install sources
- https://raw.githubusercontent.com/badlogic/pi-mono/main/packages/coding-agent/docs/security.md — project trust gate, persisted trust decisions
- https://docs.npmjs.com/policies/unpublish — version immutability, deprecate-vs-unpublish
- https://github.blog/security/supply-chain-security/our-plan-for-a-more-secure-npm-supply-chain/ — provenance, trusted publishing, post-worm hardening
- https://www.cisa.gov/news-events/alerts/2025/09/23/widespread-supply-chain-compromise-impacting-npm-ecosystem — Shai-Hulud, 500+ packages
- https://www.truesec.com/hub/blog/glassworm-first-self-propagating-worm-using-invisible-code-hits-openvsx-marketplace — invisible Unicode, 35,800 downloads, auto-update amplification
- https://docs.crates.io/crates-io/categories-and-keywords — permanence, yank semantics, ownership
- https://docs.rs/about — sandboxed per-publish doc builds
- https://raw.githubusercontent.com/microsoft/vscode-docs/main/api/working-with-extensions/publishing-extension.md — manifest, PAT retirement
- https://zed.dev/docs/extensions/developing-extensions — PR-gated publishing, weeks-long queue
- https://obsidian.md/blog/future-of-plugins/ — automated per-version review, scorecards, disclosures
- https://www.elastic.co/security-labs/phantom-in-the-vault — legitimate-plugin abuse via synced config
- https://docs.brew.sh/Acceptable-Formulae — immutable tagged releases, SHA-256, no self-update
- https://raw.githubusercontent.com/neovim/neovim/master/runtime/doc/lua-plugin.txt — LuaCATS recommendation, no manifest needed
- https://raw.githubusercontent.com/neovim/neovim/master/runtime/doc/pack.txt — `vim.pack` lockfile + confirmation-buffer update UX
- https://github.com/folke/lazy.nvim — lockfile pinning, spec format
- https://luals.github.io/wiki/export-docs/ — `lua-language-server --doc`, `doc.json` structure
- https://wezterm.org/ — MkDocs Material precedent, per-function Lua reference pages
- https://luarocks.org/ — 2019 incident: no signing or verification
- Docs `12` (trust boundary), `18` (API versioning), `19` (extension lifecycle), `22` (TOML config), `24` (packaging and signing)
