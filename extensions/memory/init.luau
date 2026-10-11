--!strict
-- The memory_search tool: greps the user-authored notes ledger.
--
-- The ledger is written by the human, never by the model, so this is a read
-- path only. It takes a QUERY, never a path: the host resolves it against the
-- one ledger for this workspace, so the tool cannot become an arbitrary-file
-- read. Same shape as `skill_read`.

local loaded: boolean = pcall(function()
	mcode.tool.register({
		name = "memory_search",
		description = "Search the project's notes ledger -- dated entries the user "
			.. "recorded about this project and their preferences. Use it before "
			.. "repeating an approach you are unsure about.",
		permission = "read",
		schema = {
			["type"] = "object",
			properties = {
				query = {
					["type"] = "string",
					description = "Text to match against the notes, case-insensitively",
				},
			},
			required = { "query" },
		},
		run = function(args: { [string]: any }, _context: {
			session: string,
			seq: number,
			extension: string,
		}): (string?, string?)
			local query = args.query
			if query == nil or query == "" then
				return nil, "query is required"
			end

			if type(query) ~= "string" then
				return nil, "query must be a string"
			end

			return mcode.memory.search(query)
		end,
	})
end)

if not loaded then
	mcode.log.error("memory_search could not be registered")
end
