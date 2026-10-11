--!strict
-- The todo tool: plan tracking over session state.
--
-- This is the smallest complete extension in the shipping set, and it is the
-- example an author reads first, so it stays small on purpose.
--
-- It holds NO state of its own. Each call supplies the whole list, which is
-- rendered and handed to `mcode.plan.set`; the loop owns it from there, and
-- recitation reads it from session state on its own interval. That is why the
-- plan survives this extension being disabled -- the tool disappears, the plan
-- does not -- and why there is no second copy to drift out of step.

local set_plan = mcode.plan.set

if set_plan == nil then
	mcode.log.error(
		"the plan sink is missing, so the todo tool cannot record a plan; "
			.. "recitation will carry only the task"
	)
	return
end

local STATUS_MARKS: { [string]: string } = {
	pending = "[ ]",
	in_progress = "[~]",
	completed = "[x]",
}

local registered: boolean = pcall(function()
	mcode.tool.register({
		name = "todo",
		description = "Record the plan for this task so it survives context "
			.. "compaction. Call it with the WHOLE list every time: it replaces the "
			.. "previous plan, so keep finished items in it and mark them completed "
			.. "rather than dropping them. Update it when the plan changes, and keep "
			.. "exactly one item in_progress.",
		permission = "read",
		schema = {
			["type"] = "object",
			properties = {
				items = {
					["type"] = "array",
					description = "The full plan, in order",
					items = {
						["type"] = "object",
						properties = {
							text = {
								["type"] = "string",
								description = "One step, in the imperative",
							},
							status = {
								["type"] = "string",
								enum = { "pending", "in_progress", "completed" },
							},
						},
						required = { "text" },
					},
				},
			},
			required = { "items" },
		},
		run = function(args: { [string]: any }, _context: {
			session: string,
			seq: number,
			extension: string,
		}): (string?, string?)
			local items = args.items

			if type(items) ~= "table" then
				return nil, "items is required and must be an array"
			end

			if #items == 0 then
				return nil, "items is empty; send at least one step"
			end

			local lines: { string } = {}
			local in_progress = 0
			local completed = 0

			for index, item in items do
				if type(item) ~= "table" then
					return nil, "item " .. tostring(index) .. " must be a table"
				end

				local text = item.text

				if type(text) ~= "string" or text == "" then
					return nil, "item " .. tostring(index) .. " needs non-empty text"
				end

				-- An absent status is pending; an unrecognized one is a caller
				-- error, not a silent default -- a typo that quietly marked work
				-- complete would be the worst possible failure here.
				local mark = STATUS_MARKS[item.status or "pending"]

				if mark == nil then
					return nil, "item " .. tostring(index) .. " has unknown status '"
						.. tostring(item.status) .. "'; use pending, in_progress or completed"
				end

				if item.status == "in_progress" then
					in_progress = in_progress + 1
				elseif item.status == "completed" then
					completed = completed + 1
				end

				-- One line per step: the plan is recited on an interval, so every
				-- character here is charged to the prompt repeatedly.
				lines[#lines + 1] = mark .. " " .. text
			end

			if in_progress > 1 then
				return nil, "at most one item may be in_progress; "
					.. tostring(in_progress) .. " are"
			end

			local plan = table.concat(lines, "\n")

			if set_plan(plan) == nil then
				return nil, "the plan could not be recorded"
			end

			local done = tostring(completed) .. "/" .. tostring(#items) .. " complete"

			if completed == #items then
				return "plan recorded (" .. done .. "); every step is done", nil
			end

			return "plan recorded (" .. done .. ")", nil
		end,
	})
end)

if not registered then
	mcode.log.error("the todo tool could not be registered")
end
