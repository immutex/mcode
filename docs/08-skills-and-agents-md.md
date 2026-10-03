# Skills & AGENTS.md: Extension Standards for mcode

> TL;DR: Adopt the Agent Skills spec (agentskills.io) for on-demand procedural knowledge and AGENTS.md closest-wins for always-on project instructions; both are pure filesystem conventions mcode can implement with a directory scanner, a YAML frontmatter parser, and one `skill_read` tool.

## State of the field

### Agent Skills (SKILL.md)

- Origin: Anthropic engineering post "Equipping agents for the real world with Agent Skills" (Oct 2025). Now an open standard at agentskills.io, adopted by Claude Code, Codex, Gemini CLI, Cursor, VS Code and ~26 platforms per secondary sources (Strapi blog — secondary claim).
- Format (agentskills.io/specification, fetched): a skill is a directory with a required `SKILL.md` (YAML frontmatter + Markdown body) plus optional `scripts/`, `references/`, `assets/`.
  - `name`: required, 1–64 chars, `[a-z0-9-]`, no leading/trailing/consecutive hyphens, must match parent dir name.
  - `description`: required, ≤1024 chars, must say *what it does* and *when to use it*.
  - Optional: `license`, `compatibility` (≤500 chars), `metadata` (string map), `allowed-tools` (experimental, space-separated pre-approved tools).
- Progressive disclosure (Anthropic engineering blog + platform docs):
  | Level | Content | Loaded | Token cost |
  |---|---|---|---|
| 1 | frontmatter `name`+`description` | always, at startup | **~100 tok/skill** (platform docs; the Claude blog's ~50 is the optimistic end). Budget: **≤1.5K tokens total** — ~15 skills at full description length, or more with truncated descriptions (`05` §Session start budget) |
  | 2 | `SKILL.md` body | when triggered | <5k tok recommended, ≤500 lines |
  | 3 | references/scripts/assets | on demand | unbounded, pay-per-use |
- Anthropic's framing: metadata = table of contents, body = chapter, linked files = appendices. Scripts handle deterministic work (parse, validate, convert) without loading their source into context.
- Discovery in Claude Code (code.claude.com/docs/en/skills, fetched): skill dirs scanned at `~/.claude/skills/`, `<project>/.claude/skills/`, nested `<subdir>/.claude/skills/`, plugin bundles (`/plugin-name:skill-name`), enterprise managed dirs, `--add-dir` dirs. Nested skills load JIT — only after Claude touches a file in that subtree. Same-name collision: enterprise > personal > project; plugin skills stay namespaced so both load. `disable-model-invocation: true` hides a skill from auto-discovery (zero recurring cost, manual `/name` only). Live reload via filesystem watch.
- Custom commands merged into skills: `.claude/commands/deploy.md` and `.claude/skills/deploy/SKILL.md` both create `/deploy`. Frontmatter extras: `!`cmd`` dynamic context injection (shell output inlined before model sees skill), `@file` attachments, `paths`-style scoping — Claude Code extensions, not part of the standard.
- Ecosystem: `anthropics/skills` (official repo, doubles as a plugin marketplace via `.claude-plugin/marketplace.json`), `VoltAgent/awesome-agent-skills` (curated index of 1000+ skills), various community marketplaces (untrusted third-party code by default).
- Skills vs MCP: skills = static instructions + files executed *inside* the agent's existing principal and tools (no new process, no new auth); MCP = live external servers with their own identity, transport, tokens, lifecycle. Rule of thumb (waltlabs framework, Ronacher's "Skills vs Dynamic MCP Loadouts"): need a different security principal or live external connectivity → MCP; procedural knowledge within the agent's own permissions → skill. Ronacher further argues skill summaries are more token-stable than dynamic MCP tool loading, whose tool descriptions drift and burn budget unpredictably.
- Other harnesses' equivalents:
  | Harness | Mechanism | Notes |
  |---|---|---|
  | OpenAI Codex | `~/.codex/prompts/*.md` custom prompts → `/prompts:name`; filename = command; `argument-hint` frontmatter, `$ARGUMENTS` | No project-scoped prompts yet (open issue); no skill-body/progressive-disclosure concept |
  | Cursor | `.cursor/rules/*.mdc` with `description`/`globs`/`alwaysApply` frontmatter; AGENTS.md supported as simpler layer | Activation matrix: alwaysApply → always in ctx; globs → when matching file in ctx; description-only → model decides; neither → only on @-mention |
  | Gemini CLI | TOML-configurable context filenames, custom commands | Concatenation model, no formal override semantics |

### AGENTS.md

- Origin: introduced 2025 by OpenAI Codex, Amp, Google Jules, Cursor, Factory; now stewarded by the Agentic AI Foundation under the Linux Foundation (agents.md, fetched).
- Format: plain Markdown, no required fields or schema. "README for agents": build/test commands, conventions, architecture, security constraints. 60k+ open-source projects contain one (agents.md's GitHub-search-linked claim).
- Adoption (all confirmed in vendor docs or agents.md): OpenAI Codex, Jules, Factory, Aider, goose, opencode, Zed, Warp, VS Code, Devin, UiPath, Junie, Cursor, Amp, RooCode, Gemini CLI, Kilo, Semgrep, Copilot, Windsurf, Augment.
- Nesting: root + nested per-subproject `AGENTS.md`. agents.md FAQ: "the closest AGENTS.md to the edited file wins; explicit user chat prompts override everything." OpenAI's monorepo reportedly has 88 AGENTS.md files.
- Size guidance: no formal spec limits. Claude Code guidance (fetched) targets <200 lines per memory file for adherence. Codex caps per-file read at `project_doc_max_bytes` (default 32768).
- Per-harness load semantics (vendor docs):
  | Harness | Behavior |
  |---|---|
  | Codex | `~/.codex/AGENTS.override.md` → `~/.codex/AGENTS.md` → repo root → cwd, appended in that order; nested appended later = higher precedence; `AGENTS.override.md` beats `AGENTS.md` in same dir; fallback filenames configurable |
  | Amp | cwd + parents (to `$HOME`) always; subtree files JIT when agent reads a file there; system/org files always; falls back to `AGENT.md`/`CLAUDE.md` per dir; `@-mention` includes other files; `globs:` frontmatter in mentioned files scopes them |
  | Claude Code | loads CLAUDE.md from cwd + all ancestors, concatenates root→cwd (closest read last); subdir files JIT on file access; `AGENTS.md` read directly or alongside |
  | Gemini CLI | hierarchical discovery + concatenation, no documented override winner |
  | Cursor | nested AGENTS.md combined, "more specific takes precedence" per docs |
- Sibling files: `CLAUDE.md` (Claude Code; adds `@path` imports, max 4 hops deep, `.claude/rules/` with `paths:` glob scoping, managed-policy location), `GEMINI.md` (Gemini CLI; `~/.gemini/GEMINI.md` global + project + subtree, configurable filename), `.cursorrules` (legacy Cursor, superseded by `.cursor/rules/*.mdc`).

## What works / what doesn't

| Approach | Result |
|---|---|
| Metadata-only always-on routing | Works, but bounded. ~100 tok/skill means the 1.5K skill-index budget covers ~15 skills at full description length; bodies cost 0 until used |
| Loading full skill bodies eagerly | Doesn't. 500-line skills × N skills would eat 10k+ tok before the first user message |
| Closest-wins nested instructions | Works (agents.md FAQ; Codex append-order). Predictable, monorepo-friendly |
| Pure concatenation without precedence | Doesn't reliably. Claude Code users complain merged globals+project+subdir lose "closest wins" defaults (claude-code issue #6235); Gemini docs admit no conflict algorithm — contradictions resolve arbitrarily |
| Instructions as enforcement | Doesn't. Claude Code docs: memory files are context, not configuration; use hooks to actually block actions |
| Skills as a substitute for external connectivity | Doesn't. Skills cannot open live authenticated sessions; that is MCP's job |
| Community skills unreviewed | Risk. Skills bundle executable scripts; Anthropic's own docs flag auditing scripts/network access before enabling |

## Recommended design for mcode

**Principles:** skills and instruction files are *data* — no new runtime, no interpreter. The model reads them with mcode's existing file tools. Progressive disclosure is enforced by the harness only at level 1 (index injection); levels 2–3 are ordinary `read`/shell calls gated by existing permissions.

### Instruction-file discovery (AGENTS.md)

1. At startup, walk from cwd up to the filesystem root (stop early at a detected repo root — `.git`). In each directory pick **one** file: `AGENTS.md`, else `CLAUDE.md`, else `GEMINI.md` (first match wins per dir).
2. Prepend user-global `$XDG_CONFIG_HOME/mcode/AGENTS.md` (`~/.config/mcode/`, or `%APPDATA%\mcode\` on Windows / `~/Library/Application Support/mcode` on macOS — `24`) if present; prepend org-managed `<mcode-install>/AGENTS.md` if present.
3. Concatenate ordered: org → user → root → … → cwd. Closest file appears last in context (matches Claude Code's root-down ordering and Codex's append-precedence).
4. **Overflow safety limits, not budgets**: hard-stop reading at 32 KiB per file (Codex's `project_doc_max_bytes`) and 64 KiB per chain, truncating the *broadest* file first with a one-line pointer so the model can `read` the rest on demand. The real budget is **≤2K tokens for the whole chain** (`05` §Numeric guidance). The chain is enforced in **two passes**: the 64 KiB byte cap first (a safety bound against a pathological file), then the 2K-token cap. Both cut from the **broadest end** and stop before the last entry, so the closest file — the one the model most needs — is never truncated, and the cut leaves a visible `[truncated: over the instruction-chain budget; read <path> for the rest]` pointer rather than a silent loss. The research is unambiguous that a bloated chain buys no success rate and costs >20% more inference (arXiv 2602.11988): prune anything the model can infer.
5. Nested/subtree files: JIT. On first `read`/`edit` of a file under a subdirectory containing an `AGENTS.md` not yet loaded, inject that file (8 KiB cap) as a system note, once per session. This is Claude Code's and Amp's documented behavior and it is correct — startup cost stays flat in monorepos.
6. Conflict resolution: **closest wins** (agents.md FAQ). Later-in-chain file overrides earlier on contradiction; user chat overrides everything. mcode states this rule verbatim in its system prompt so the model applies it.
7. No import/expansion syntax in v1. Amp's `@-mention` and Claude Code's `@path` imports pull whole files into every session — a budget hazard. If added later, cap depth at 4 hops (Claude Code's limit) and expand only on activation.

### Skill discovery & activation

1. Scan at startup: `<user-config>/mcode/skills/*/SKILL.md`, `<repo>/.mcode/skills/*/SKILL.md`, nested `<subdir>/.mcode/skills/*/SKILL.md`, and `plugins/*/skills/*/SKILL.md` for enabled plugins. Also accept `.claude/skills/` and `AGENTS.md`-ecosystem dirs read-only for compatibility.
2. Parse frontmatter only (cheap: read first 4 KiB, split on `---`). Validate `name` regex `^[a-z0-9]+(-[a-z0-9]+)*$`, ≤64 chars, matches dir name; `description` ≤1024 chars. Invalid → skip + log, never crash startup.
3. Name collision: project > user > plugin; plugin skills additionally reachable as `plugin:skill` (always unambiguous, mirrors Claude Code namespacing). Explicit `skillOverrides` map in mcode config wins over all defaults.
4. Inject an index into the system prompt: one line per skill, `name — description` (description truncated to 200 chars for routing; full description available on demand). Reserved names: `synced`, `mcode`.
   
   **The index is emitted only when the `skills` extension is loaded** (`23`). Discovery on disk is independent of the extension, so a disabled `skills` extension must not leave an index in the prompt pointing at a `skill_read` tool the model does not have — the dangling-reference class `21` §Conditional assembly forbids. With the extension disabled, skills remain reachable by explicit `/name` only.
5. Activation, two paths: (a) user types `/name` — deterministic; (b) model calls the `skill_read(name)` tool — the *only* mechanism for auto-invocation. Never preload bodies.
6. On activation: inject `SKILL.md` body (cap 16 KiB read; warn the user above 500 lines / ~5k tok). Honor `allowed-tools` frontmatter by pre-approving matching tool patterns for that turn only — everything else goes through normal permission gates.
7. Level 3: body references files relative to the skill root; the model reads them with the normal `read` tool and runs `scripts/` through the normal shell tool with normal confirmation. mcode adds nothing here — deliberately.
8. Watch skill dirs (and instruction files) with a filesystem watcher; re-scan on change without restart (Claude Code parity, trivial with std::filesystem + a debounce).
9. Optional `disable-model-invocation: true` frontmatter (Claude Code extension worth adopting): skill invisible to routing, `/name` only — zero recurring token cost for side-effectful skills.

### Composition with tools

Skills never register tools. A skill that needs live data should *instruct the model to call an MCP tool* or run a script; the skill supplies the procedure, MCP supplies the principal/transport. This keeps the tool schema set static and token-stable (Ronacher's core argument) while skills stay pure Markdown.

**Skills versus Lua extensions** — different jobs, deliberately not merged:

| | Skill | Lua extension |
|---|---|---|
| Contains | Instructions, references, optional scripts | Executable logic, tools, hooks |
| Cost when unused | ~30 tokens (metadata only) | 0 tokens; ~0 ms load (`19`) |
| Written by | Anyone who can write Markdown | Anyone who can write Lua |
| Can add a tool? | No — instructs the model to use existing ones | Yes — registers into the tool registry |
| Can veto an action? | No | Yes, on `Pre*` events (`18`) |

The rule: **if it changes what the agent can *do*, it is an extension; if it changes what the agent *knows*, it is a skill.** Skills are the low-ceremony path and should stay that way — pushing a skill into Lua when it only needed instructions adds a failure mode for no gain.

A Lua extension *may* ship skills in its directory; discovery scans extension directories as additional skill roots (`19`).

- **Hot reload is a session-start event, not a mid-session one.** A filesystem watcher re-scans skills and instruction files, but a detected change does **not** rewrite the live prompt — it stages the new chain and offers `/reload` (or applies it at the next session boundary). Mid-session prompt mutation would invalidate the cached prefix from the mutation point (`15`) and contradict the byte-stability invariant in `03`. The `prompt.pre` hook is the one sanctioned per-turn amendment and it appends to the **volatile tail**, never the prefix (`18`, `20`).

## Traps

- **Description bloat.** The description *is* the routing key. "Helps with finance." routes nothing; agentskills.io's own good/bad examples make specificity mandatory. Enforce the "Use when…" clause in `mcode skill validate`.
- **Merging instead of precedence.** Concatenate-all-and-hope (Gemini, Claude Code's global+project merge criticized in issue #6235) produces contradictions the model resolves arbitrarily. Pick closest-wins and state it in the system prompt.
- **Eager body loading.** Any harness that preloads SKILL.md bodies pays N×5k tok forever. Routing metadata only.
- **Instructions treated as enforcement.** A prompt can be ignored; a PreToolUse hook cannot. mcode must route "never do X" safety requirements to hooks, not AGENTS.md.
- **Skills accreting into MCP servers.** Credentials, persistent network access, or independent identity in a skill directory is a design smell (waltlabs framework) — point users at MCP.
- **Prompt injection via frontmatter.** Descriptions are attacker-controllable text in the system prompt for anyone installing a community skill. Sanitize control characters and escape `<`/`>` (Claude Code does this for synced skills; mcode should do it for all).
- **Symlink loops / duplicate reads.** Claude Code dedupes symlinked skill dirs and forbids network-path symlinks. mcode must resolve+dedupe by canonical path and cap scan depth.
- **Brace-expansion blowups.** Claude Code had startup stalls/crashes from pathological `paths:` globs before adding a 1,000-pattern/4 MiB budget (v2.1.217). If mcode ever adds glob-scoped rules, budget the expansion up front.

## Open questions

- Skill update semantics: re-read body on every activation (fresh but token-costly across a long session) vs cache with mtime invalidation? Lean cache+mtime.
- Should `skill_read` results persist in conversation history or be re-injectable after compaction? Claude Code keeps them as conversation content; compaction may drop them mid-procedure.
- Automatic skill *authoring*: Claude Code's `/run-skill-generator` records a working build/launch recipe as a committed skill. Worth copying once mcode has real usage data.
- Routing quality at scale: does model-invoked selection degrade past ~50 skills? No published numbers found. Needs an eval harness before adding search/keyword filtering.

## Sources

- https://www.anthropic.com/engineering/equipping-agents-for-the-real-world-with-agent-skills — Anthropic engineering blog (progressive disclosure, routing model)
- https://agentskills.io/specification — Agent Skills open standard spec (fetched in full)
- https://platform.claude.com/docs/en/agents-and-tools/agent-skills/overview — field constraints, ~100 tok metadata
- https://code.claude.com/docs/en/skills — Claude Code skill locations, collisions, JIT nested loading, frontmatter extensions (fetched)
- https://code.claude.com/docs/en/memory — CLAUDE.md load order, 200-line guidance, imports, rules (fetched)
- https://agents.md/ — AGENTS.md standard, adoption list, nesting FAQ, AAIF stewardship (fetched)
- https://ampcode.com/docs/customize/agents-md — Amp hierarchy, fallbacks, globs (fetched)
- https://developers.openai.com/codex/guides/agents-md + https://github.com/openai/codex/blob/main/codex-rs/core/src/agents_md.rs — Codex chain order, override files (via search excerpts)
- https://github.com/google-gemini/gemini-cli/blob/main/docs/cli/gemini-md.md — GEMINI.md concatenation semantics (via search excerpts)
- https://cursor.com/docs/rules — `.mdc` activation matrix, AGENTS.md support (via search excerpts)
- https://github.com/anthropics/claude-code/issues/6235 — merge-vs-closest-wins complaint
- https://lucumr.pocoo.org/2025/12/13/skills-vs-mcp/ — Ronacher on skill vs dynamic MCP loading stability
- https://waltlabs.io/blog/skill-vs-mcp-server-vs-plugin-claude-decision-framework — principal/stateless decision framework (secondary)
- https://github.com/anthropics/skills, https://github.com/VoltAgent/awesome-agent-skills — official + community skill repos
