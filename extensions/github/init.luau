--!strict
-- GitHub tools over the `gh` CLI: gh_pr, gh_issue and gh_review.
--
-- The `gh` CLI rather than the REST API, because it inherits the user's existing
-- auth and repository context -- an API client would need its own token, its own
-- pagination and its own rate-limit handling, and would still be wrong about
-- which repository the user means.
--
-- Two things this demonstrates that the other extensions do not:
--
--   1. A credentialed external service, where the credential is the CLI's and
--      never passes through this code.
--   2. Error mapping: `gh` says useful things on stderr, and those are what the
--      model needs to see, not a generic "command failed".
--
-- Every argument reaches the command line through the same quoting helper the
-- `git` extension uses, and nothing here ever passes a free-form string to a
-- shell.

local run = mcode.bash

if run == nil then
	mcode.log.error(
		"the command runner is missing, so the GitHub tools cannot run; "
			.. "the agent will have to shell out to gh itself"
	)
	return
end

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

local function shell(command: string): (string?, string?)
	local result, err = run(command, { timeout_ms = 120000 })

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

	-- `gh` puts the reason on stderr: "not logged in", "no pull requests found",
	-- a 404 naming the repository. Replacing that with a status code would throw
	-- away the only part that tells the model what to do next.
	local detail = stderr

	if type(detail) ~= "string" or detail == "" then
		detail = stdout
	end

	if type(detail) ~= "string" or detail == "" then
		detail = "gh exited " .. tostring(result.exit_code)
	end

	return nil, detail
end

local registered: boolean = pcall(function()
	mcode.tool.register({
		name = "gh_pr",
		description = "Read or open a pull request. With no arguments, lists the "
			.. "open ones. Give number to read one in full, including its diff. "
			.. "Give title and head to open a new one.",
		permission = "exec",
		schema = {
			["type"] = "object",
			properties = {
				number = {
					["type"] = "integer",
					description = "The pull request to read",
				},
				title = {
					["type"] = "string",
					description = "Title for a new pull request",
				},
				head = {
					["type"] = "string",
					description = "The branch a new pull request comes from",
				},
				base = {
					["type"] = "string",
					description = "The branch a new pull request targets",
				},
				body = {
					["type"] = "string",
					description = "Body for a new pull request",
				},
			},
		},
		run = function(args: { [string]: any }, _context: any): (string?, string?)
			if args.number ~= nil then
				if type(args.number) ~= "number" then
					return nil, "number must be an integer"
				end

				return shell("gh pr view " .. tostring(math.floor(args.number)) .. " --comments")
			end

			if args.title ~= nil then
				if type(args.title) ~= "string" or args.title == "" then
					return nil, "title must be a non-empty string"
				end

				local parts = { "gh", "pr", "create", "--title" }

				local safe = quoted_all({ args.title })

				if safe == nil then
					return nil, "title contains a byte that cannot be quoted"
				end

				parts[#parts + 1] = safe[1]

				if args.head ~= nil then
					if type(args.head) ~= "string" then
						return nil, "head must be a string"
					end

					local head = quoted_all({ args.head })

					if head == nil then
						return nil, "head contains a byte that cannot be quoted"
					end

					parts[#parts + 1] = "--head"
					parts[#parts + 1] = head[1]
				end

				if args.base ~= nil then
					if type(args.base) ~= "string" then
						return nil, "base must be a string"
					end

					local base = quoted_all({ args.base })

					if base == nil then
						return nil, "base contains a byte that cannot be quoted"
					end

					parts[#parts + 1] = "--base"
					parts[#parts + 1] = base[1]
				end

				if args.body ~= nil then
					if type(args.body) ~= "string" then
						return nil, "body must be a string"
					end

					local body = quoted_all({ args.body })

					if body == nil then
						return nil, "body contains a byte that cannot be quoted"
					end

					parts[#parts + 1] = "--body"
					parts[#parts + 1] = body[1]
				end

				return shell(table.concat(parts, " "))
			end

			return shell("gh pr list --limit 20")
		end,
	})

	mcode.tool.register({
		name = "gh_issue",
		description = "Read or open an issue. With no arguments, lists the open "
			.. "ones. Give number to read one in full. Give title to open a new one.",
		permission = "exec",
		schema = {
			["type"] = "object",
			properties = {
				number = {
					["type"] = "integer",
					description = "The issue to read",
				},
				title = {
					["type"] = "string",
					description = "Title for a new issue",
				},
				body = {
					["type"] = "string",
					description = "Body for a new issue",
				},
			},
		},
		run = function(args: { [string]: any }, _context: any): (string?, string?)
			if args.number ~= nil then
				if type(args.number) ~= "number" then
					return nil, "number must be an integer"
				end

				return shell("gh issue view " .. tostring(math.floor(args.number)) .. " --comments")
			end

			if args.title ~= nil then
				if type(args.title) ~= "string" or args.title == "" then
					return nil, "title must be a non-empty string"
				end

				local safe = quoted_all({ args.title })

				if safe == nil then
					return nil, "title contains a byte that cannot be quoted"
				end

				local command = "gh issue create --title " .. safe[1]

				if args.body ~= nil then
					if type(args.body) ~= "string" then
						return nil, "body must be a string"
					end

					local body = quoted_all({ args.body })

					if body == nil then
						return nil, "body contains a byte that cannot be quoted"
					end

					command = command .. " --body " .. body[1]
				end

				return shell(command)
			end

			return shell("gh issue list --limit 20")
		end,
	})

	mcode.tool.register({
		name = "gh_review",
		description = "Read a pull request's review state and its inline comments, "
			.. "so you can see what a reviewer asked for before changing anything.",
		permission = "exec",
		schema = {
			["type"] = "object",
			properties = {
				number = {
					["type"] = "integer",
					description = "The pull request to read the reviews of",
				},
			},
			required = { "number" },
		},
		run = function(args: { [string]: any }, _context: any): (string?, string?)
			local number = args.number

			if type(number) ~= "number" then
				return nil, "number is required and must be an integer"
			end

			local shown = tostring(math.floor(number))
			local comments, comments_error = shell("gh pr view " .. shown .. " --comments")

			if comments == nil then
				return nil, comments_error
			end

			local checks, _ = shell("gh pr checks " .. shown)

			if checks == nil then
				return comments
			end

			return comments .. "\n\n--- checks ---\n" .. checks
		end,
	})
end)

if not registered then
	mcode.log.error("the GitHub tools could not be registered")
end
