--!strict
-- Git tools: git_status, git_diff and git_commit.
--
-- A workflow tool rather than a CRUD mirror, and the reference for composing
-- `mcode.bash`. Two rules it demonstrates:
--
--   1. It runs git through the core `bash` handler, so the sandbox profile, the
--      permission gate and the output cap are the ones every other command gets.
--      There is no second exec path, and nothing here can reach around the gate.
--   2. Arguments reach the command line through a shell-quoting helper, never by
--      concatenation. A path with a space or a semicolon is data, not syntax.
--
-- `git_commit` is the interesting one: it is the only tool here that changes the
-- repository, so it shows the write path and a message the model does not get to
-- write unchecked.

local run = mcode.bash

if run == nil then
	mcode.log.error(
		"the command runner is missing, so the git tools cannot run; "
			.. "the agent will have to shell out to git itself"
	)
	return
end

-- Single-quote for POSIX shells, and refuse anything that cannot be quoted
-- safely rather than guessing. The sandbox runs a shell, so a value that reaches
-- it unquoted is a command.
local function quote(value: string): string?
	if value:find("\0") ~= nil then
		return nil
	end

	return "'" .. value:gsub("'", "'\\''") .. "'"
end

local function quoted_all(values: { string }): { string }?
	local out: { string } = {}

	for _, value in values do
		local safe = quote(value)

		if safe == nil then
			return nil
		end

		out[#out + 1] = safe
	end

	return out
end

-- `mcode.bash` returns the core handler's own result, decoded, so the fields are
-- the ones the model's `bash` tool produces: `succeeded` for the outcome (not
-- `ok`, which only means the command ran), and stdout/stderr kept apart.
local function shell(command: string): (string?, string?)
	local result, err = run(command, { timeout_ms = 60000 })

	if result == nil then
		return nil, err
	end

	local stdout = result.stdout
	local stderr = result.stderr

	if result.succeeded == true then
		if type(stdout) ~= "string" or stdout == "" then
			return "(no output)"
		end

		return stdout
	end

	-- git writes its diagnostics to stderr; a failure that reported only stdout
	-- would tell the caller nothing.
	local detail = stderr

	if type(detail) ~= "string" or detail == "" then
		detail = stdout
	end

	if type(detail) ~= "string" or detail == "" then
		detail = "git exited " .. tostring(result.exit_code)
	end

	return nil, detail
end

local registered: boolean = pcall(function()
	mcode.tool.register({
		name = "git_status",
		description = "Show the working tree status: which files are modified, "
			.. "staged, untracked or conflicted. Run it before deciding what to "
			.. "commit, and after an edit to confirm what changed.",
		permission = "exec",
		schema = {
			["type"] = "object",
			properties = {
				short = {
					["type"] = "boolean",
					description = "Use --short for one line per file",
				},
			},
		},
		run = function(args: { [string]: any }, _context: any): (string?, string?)
			local command = "git status"

			if args.short == true then
				command = command .. " --short"
			end

			return shell(command)
		end,
	})

	mcode.tool.register({
		name = "git_diff",
		description = "Show changes. With no arguments, the unstaged changes to "
			.. "tracked files. Use staged=true to see what a commit would capture, "
			.. "or path to narrow it to one file.",
		permission = "exec",
		schema = {
			["type"] = "object",
			properties = {
				path = {
					["type"] = "string",
					description = "Limit the diff to this repository-relative path",
				},
				staged = {
					["type"] = "boolean",
					description = "Diff the index against HEAD instead of the worktree",
				},
				stat = {
					["type"] = "boolean",
					description = "Show a summary of changed lines per file",
				},
			},
		},
		run = function(args: { [string]: any }, _context: any): (string?, string?)
			local command = "git --no-pager diff"

			if args.staged == true then
				command = command .. " --cached"
			end

			if args.stat == true then
				command = command .. " --stat"
			end

			if args.path ~= nil then
				if type(args.path) ~= "string" or args.path == "" then
					return nil, "path must be a non-empty string"
				end

				local safe = quote(args.path)

				if safe == nil then
					return nil, "path contains a byte that cannot be quoted"
				end

				command = command .. " -- " .. safe
			end

			return shell(command)
		end,
	})

	mcode.tool.register({
		name = "git_commit",
		description = "Stage the named paths and commit them. Pass the paths "
			.. "explicitly: `git add -A` would sweep in files the user did not ask "
			.. "you to touch. The message is the commit's whole content, so write it "
			.. "as one imperative line saying why, not what.",
		permission = "exec",
		schema = {
			["type"] = "object",
			properties = {
				message = {
					["type"] = "string",
					description = "One imperative line, saying why the change was made",
				},
				paths = {
					["type"] = "array",
					description = "Repository-relative paths to stage and commit",
					items = { ["type"] = "string" },
				},
			},
			required = { "message", "paths" },
		},
		run = function(args: { [string]: any }, _context: any): (string?, string?)
			local message = args.message

			if type(message) ~= "string" or message == "" then
				return nil, "message is required and must be a non-empty string"
			end

			if message:find("\n") ~= nil then
				return nil, "message must be a single line"
			end

			local paths = args.paths

			if type(paths) ~= "table" or #paths == 0 then
				return nil, "paths is required and must name at least one path"
			end

			local quoted = quoted_all(paths)

			if quoted == nil then
				return nil, "a path contains a byte that cannot be quoted"
			end

			local safe_message = quote(message)

			if safe_message == nil then
				return nil, "message contains a byte that cannot be quoted"
			end

			local staged, stage_error = shell("git add -- " .. table.concat(quoted, " "))

			if staged == nil then
				return nil, "staging failed: " .. stage_error
			end

			return shell("git commit -m " .. safe_message)
		end,
	})
end)

if not registered then
	mcode.log.error("the git tools could not be registered")
end
