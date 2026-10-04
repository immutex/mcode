--!strict
-- mcode extension API, version 1.
--
-- Author-facing definition file. Drop this next to your extension (or reference
-- it from `.luaurc` via `aliases`) and `luau-analyze` will type-check and
-- complete every `mcode.*` call.
--
-- The surface is frozen at api_version 1. Every name here is a permanent
-- forward-compatibility commitment: entries are added in later versions, never
-- removed or renamed. `mcode.capabilities` exists for feature detection.
--
-- Conventions (see docs/18):
--   * Environmental failure returns `value, err` -- assert it.
--   * Contract violation raises -- pcall it if you might be wrong.
--   * Permission denial is environmental, so it returns `nil, err`.
--   * Every call is bounded; none blocks indefinitely.

declare mcode: {
	-- The API version this host implements. Compare against a floor:
	--   if mcode.api_version < 2 then ... end
	api_version: number,

	-- Optional features, for detecting additions without sniffing versions.
	capabilities: { [string]: boolean },

	-- This extension's own manifest name. Use it for self-attribution.
	ext: {
		name: string,
	},

	tool: {
		-- Registers a tool. A name collision is a contract violation and raises.
		--
		-- `schema` is a JSON Schema object; it is validated here and rendered
		-- byte-stably for prompt caching, so do not build it dynamically.
		--
		-- `run` receives the raw argument table and a context table, and returns
		-- a string, or `nil, err` for an environmental failure.
		--
		-- `permission` is the tool's class in the permission engine, exactly as
		-- a built-in declares one. It decides what the call is checked against:
		-- `read`/`write` key on the `path` argument, `exec` on `command`, and
		-- `net`/`spawn` always prompt. Omitting it is not "read": an undeclared
		-- tool prompts on every call, because the read default would otherwise
		-- auto-approve it inside the workspace.
		register: (definition: {
			name: string,
			description: string,
			schema: { [string]: any },
			permission: ("read" | "write" | "exec" | "net" | "spawn")?,
			run: (args: { [string]: any }, context: {
				session: string,
				seq: number,
				extension: string,
			}) -> (string?, string?),
		}) -> number,

		-- Unregisters a tool by the id `register` returned. Deferred if the tool
		-- is mid-dispatch. A bad id is a contract violation and raises.
		unregister: (id: number) -> (),
	},

	cmd: {
		-- Registers a slash command. `opts.complete` is optional and returns
		-- candidate completions for the argument prefix.
		register: (name: string, run: (args: string) -> (), opts: {
			description: string?,
			complete: ((prefix: string) -> { string })?,
		}?) -> number,
	},

	-- Subscribes to an event. Returns a subscription id.
	--
	-- Handlers are SYNCHRONOUS and run on the loop thread under a 50 ms
	-- wall-clock budget. Only the vetoable events (tool.pre_call, spawn.pre,
	-- prompt.pre) accept a veto return.
	--
	-- Returning `{ veto = "reason" }` blocks the action and surfaces the reason
	-- to the model, attributed to this extension. First veto wins and
	-- short-circuits the remaining handlers.
	--
	-- A handler that throws is contained and counted; 5 consecutive failures
	-- quarantine the extension.
	on: (
		event: string,
		-- Returning nothing at all is a type error under --!strict, so every
		-- handler ends with an explicit `return nil` on the path where it does not
		-- veto. That is the price of typing the veto: with the return declared
		-- `any`, a misspelled `{veto = ...}` would compile and the guard the author
		-- believes is active would silently not block.
		handler: (event: { [string]: any }) -> ({ veto: string }?),
		opts: { once: boolean? }?
	) -> number,

	-- Unsubscribes by the id `on` returned.
	off: (id: number) -> (),

	-- Fires a custom event. Payload must be plain data -- no functions, no
	-- userdata, no cycles.
	emit: (event: string, payload: { [string]: any }) -> (),

	-- Defers `fn` to the next loop iteration. Use this to leave a callback
	-- context before touching host state.
	defer: (fn: () -> ()) -> (),

	timer: {
		-- Fires once after `ms`. Returns a handle; `handle:stop()` cancels.
		-- Handles are host-anchored and survive garbage collection.
		at: (ms: number, fn: () -> ()) -> { stop: (self: any) -> () },

		-- Fires every `ms` until stopped.
		every: (ms: number, fn: () -> ()) -> { stop: (self: any) -> () },
	},

	log: {
		-- Prefixed with the extension name automatically.
		debug: (...any) -> (),
		info: (...any) -> (),
		warn: (...any) -> (),
		error: (...any) -> (),
	},

	-- A user-visible, attributed message.
	notify: (message: string, level: string?) -> (),

	cfg: {
		-- Reads merged configuration for this extension, validated against the
		-- manifest. Returns `default` when the key is absent.
		get: (key: string, default: any) -> any,
	},

	session: {
		-- A plain-data copy of session state. Never a live reference.
		snapshot: () -> {
			session: string,
			seq: number,
			steps: number,
			goal: string?,
		},

		-- Branches the event log at `at_seq`. Append-only: records a
		-- branch.created event and copies nothing. Requires the `session_fork`
		-- permission.
		fork: (at_seq: number, label: string) -> (number?, string?),
	},

	-- Deliberately absent: `spawn`. It is a manifest PERMISSION, not a declared
	-- function: `bash` and `cmd` cover the need, and a raw process-spawn API is
	-- a separate decision with its own security surface. A declared-and-absent
	-- name is the failure the surface test exists to fix, so it is removed
	-- rather than left as an aspiration.

	net: {
		-- API-level host allowlist: an extension may reach only the hosts its
		-- manifest declares. This is NOT the M6 egress proxy -- that is a
		-- process-level control; this is a policy on what extension code may
		-- ask for. Requires the `net` permission.
		get: (url: string, opts: {
			headers: { [string]: string }?,
			timeout_ms: number?,
			max_bytes: number?,
		}?) -> ({ status: number, headers: { [string]: string }, body: string }?, string?),

		-- Web search through the configured provider. Requires `net`.
		search: (query: string, opts: {
			limit: number?,
			timeout_ms: number?,
		}?) -> ({ { title: string, url: string, snippet: string } }?, string?),
	},

	fs: {
		-- Permission-checked and workspace-scoped. Requires `fs_read`.
		read: (path: string) -> (string?, string?),

		-- Requires `fs_write`. Byte-capped.
		write: (path: string, data: string, opts: { append: boolean? }?) -> (boolean?, string?),
	},

	skill: {
		-- Registers a skill by value. Discovery of SKILL.md on disk is separate.
		register: (definition: {
			name: string,
			description: string,
			body: string,
			path: string?,
		}) -> number,

		-- Reads a discovered skill's body by NAME. Never a path: the host
		-- resolves the name against the discovered set, so this cannot be turned
		-- into an arbitrary-file read. Unknown names return `nil, err`.
		read: (name: string) -> (string?, string?),

		-- Lists the discovered skills: the same rows the prompt index is built
		-- from, including skills hidden from the model by
		-- `disable-model-invocation` (their `hidden` field is true).
		list: () -> { {
			name: string,
			description: string,
			hidden: boolean,
			origin: string,
		} },
	},

	mcp: {
		-- Declares an MCP server. Requires the `mcp` permission; without it the
		-- call returns `nil, err` rather than raising.
		--
		-- The declaration does not bypass consent: the server is DISABLED until
		-- the user enables it, and its launch command requires explicit approval
		-- the first time it runs. Its tools land in the registry as
		-- `mcp__<server>__<tool>`.
		register: (definition: {
			name: string,
			transport: string,
			command: { string }?,
			url: string?,
			tools: { string }?,
		}) -> (boolean?, string?),
	},

	model: {
		-- Declares a provider. Requires the `net` permission.
		--
		-- This is DATA, not a callback: it maps the provider's JSON to the
		-- canonical event model with JSON pointers, and C++ applies it natively.
		-- HTTP, SSE framing, retry, the error taxonomy, and egress policy all stay
		-- in C++, so per-token parsing never crosses into the VM.
		--
		-- Validation is strict and happens here: a missing endpoint, a non-http
		-- URL, a pointer that is not a JSON pointer, or an `auth` block with no
		-- source is refused rather than failing at first token.
		register: (definition: {
			name: string,
			endpoint: string,

			-- Omit entirely for a local endpoint. If present, `from` and `name`
			-- are required -- an auth block without a source is a typo, not
			-- "no auth".
			auth: {
				from: string, -- "env" | "config"
				name: string,
				header: string,
				scheme: string?,
			}?,

			-- JSON pointers into one SSE event's data object. At least one of
			-- text_delta or tool_calls.args must be mapped.
			stream: {
				text_delta: string?,
				thinking_delta: string?,

				tool_calls: {
					index: string?,
					id: string?,
					name: string?,
					args: string?,
				}?,

				finish: string?,
				usage: {
					-- `in` is a reserved word, so the key is bracketed. This is also
					-- what a descriptor written in JSON uses, and the two forms must
					-- agree.
					["in"]: string?,
					out: string?,
					cached_read: string?,
					cache_write: string?,
					reasoning: string?,
				}?,

				-- SSE event names that end the stream, for providers that do not
				-- send the [DONE] sentinel.
				terminal_events: { string }?,
			},

			-- Where the canonical request fields go in the outgoing body.
			request: {
				max_output_tokens: string?,
				temperature: string?,
				response_schema: string?,
			}?,
		}) -> (boolean?, string?),
	},

	context: {
		-- Contributes to the system prompt. Budgeted and counted; requires the
		-- `context` permission. Over-budget contributions are truncated, not
		-- silently dropped.
		add_instructions: (text: string, opts: { priority: number? }?) -> (boolean?, string?),
	},
}

-- Injected by the host, not a member of `mcode`. Resolves a module against this
-- extension's own `lib/` directory and nowhere else: no parent walk, no user
-- scope, no package path, and no configuration files are read or executed.
--
--   local util = require("./lib/util")
--
-- A path resolving outside the extension root, or a require cycle, is an error.
declare function require(path: string): any
