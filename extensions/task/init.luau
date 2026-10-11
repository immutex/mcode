--!strict
-- The task tool: delegates a read-only research task to a subagent.
--
-- This is the one load-bearing bundled extension (docs/23). The shell is here
-- because a bundled extension is the example an author copies; everything that
-- matters is in the core. The child's registry is filtered to the read class
-- there, so no argument this tool accepts can widen what the child may do, and
-- the child's budget is carved from this run's remainder -- a fan-out cannot
-- outspend the run.
--
-- It exists because the parent's context is the scarce resource: a research
-- sweep burns tens of thousands of tokens to produce a two-thousand-token
-- answer, and that trajectory is worth discarding.

local delegate = mcode.task

if delegate == nil then
	mcode.log.error(
		"the subagent runner is missing, so the task tool cannot delegate; "
			.. "the agent will do this research inline"
	)
	return
end

local registered: boolean = pcall(function()
	mcode.tool.register({
		name = "task",
		description = "Delegate a read-only research task to a subagent and get "
			.. "back a short report. The subagent has read, glob and grep only: it "
			.. "cannot edit, write or run a command. Use it when a question needs "
			.. "many file reads to answer -- 'where is X implemented', 'which files "
			.. "reference Y' -- so the searches stay out of this conversation. Give "
			.. "it one clear question, and put anything it cannot look up itself in "
			.. "context.",
		permission = "read",
		schema = {
			["type"] = "object",
			properties = {
				objective = {
					["type"] = "string",
					description = "One clear question for the subagent to answer",
				},
				context = {
					["type"] = "string",
					description = "What the subagent cannot discover for itself: "
						.. "paths already known, decisions already made, terms of art",
				},
				max_steps = {
					["type"] = "integer",
					description = "Step ceiling for the subagent; 24 by default, "
						.. "64 at most",
				},
			},
			required = { "objective" },
		},
		run = function(args: { [string]: any }, _context: {
			session: string,
			seq: number,
			extension: string,
		}): (string?, string?)
			local objective = args.objective

			if type(objective) ~= "string" or objective == "" then
				return nil, "objective is required and must be a non-empty string"
			end

			local opts: { [string]: any } = {}

			if args.context ~= nil then
				if type(args.context) ~= "string" then
					return nil, "context must be a string"
				end

				opts.context = args.context
			end

			if args.max_steps ~= nil then
				if type(args.max_steps) ~= "number" then
					return nil, "max_steps must be an integer"
				end

				opts.max_steps = args.max_steps
			end

			local report = delegate(objective, opts)

			if report == nil then
				return nil, "the subagent runner is unavailable in this session"
			end

			if type(report) ~= "table" then
				return nil, "the subagent runner returned an unexpected value"
			end

			local summary = report.summary

			if type(summary) ~= "string" or summary == "" then
				return nil, "the subagent produced no report"
			end

			if report.truncated then
				summary = summary
					.. "\n[the subagent stopped before finishing, or its report "
					.. "exceeded the return budget]"
			end

			return summary
		end,
	})
end)

if not registered then
	mcode.log.error(
		"the task tool could not be registered, so delegation is unavailable "
			.. "in this session"
	)
end
