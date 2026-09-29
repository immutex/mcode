--!strict
-- The skill_read tool: the model's only way to fetch a skill body.
--
-- The harness discovers skills and injects the index into the system prompt;
-- this tool closes the loop on demand. It takes a NAME, never a path: the
-- host resolves the name against the discovered set, so the tool cannot be
-- turned into an arbitrary-file read.

local loaded: boolean = pcall(function()
	mcode.tool.register({
		name = "skill_read",
		description = "Read the full body of a skill by name. Call this when the "
			.. "skill index in the system prompt names a skill relevant to the "
			.. "current task, before following its instructions.",
		schema = {
			["type"] = "object",
			properties = {
				name = {
					["type"] = "string",
					description = "The skill's name, exactly as it appears in the index",
				},
			},
			required = { "name" },
		},
		run = function(args: { [string]: any }, _context: {
			session: string,
			seq: number,
			extension: string,
		}): (string?, string?)
			local name = args.name
			if name == nil or name == "" then
				return nil, "name is required"
			end

			if type(name) ~= "string" then
				return nil, "name must be a string"
			end

			return mcode.skill.read(name)
		end,
	})
end)

if not loaded then
	mcode.log.error("skill_read could not be registered")
end
