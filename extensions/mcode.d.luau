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
		register: (definition: {
			name: string,
			description: string,
			schema: { [string]: any },
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

	-- Runs a subprocess. Requires the `spawn` permission.
	-- Bounded by `opts.timeout_ms`; the child is killed on expiry.
	spawn: (argv: { string }, opts: {
		cwd: string?,
		timeout_ms: number?,
		stdin: string?,
	}) -> ({
		exit_code: number,
		stdout: string,
		stderr: string,
		timed_out: boolean,
	}?, string?),

	net: {
		-- Gated HTTP through the egress proxy. Requires the `net` permission.
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
	},

	mcp: {
		-- Declares an MCP server. Requires the `mcp` permission.
		-- Its tools land in the registry as `mcp__<server>__<tool>`.
		register: (definition: {
			name: string,
			transport: string,
			command: { string }?,
			url: string?,
			tools: { string }?,
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
