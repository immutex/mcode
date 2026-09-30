--!strict
-- Positive fixture: every documented call shape, type-checked against
-- extensions/mcode.d.luau. If the definition file drifts from docs/18, this
-- file stops compiling -- which is the point.

local name = mcode.ext.name
mcode.log.info("loading", name)

local tool_id = mcode.tool.register({
	name = "git_status",
	description = "Show the working tree status",
	schema = { type = "object", properties = {} },
	run = function(args, context)
		local status, err = mcode.fs.read("git-status.txt")

		if not status then
			return nil, err
		end

		return status, nil
	end,
})

mcode.cmd.register("gst", function(rest)
	local status = mcode.fs.read("git-status.txt")

	if status then
		mcode.notify(status, "info")
	end
end, { description = "git status" })

mcode.on("tool.pre_call", function(event)
	local cmd = event.args and event.args.cmd

	if type(cmd) == "string" and string.find(cmd, "^rm%s") then
		return { veto = "destructive shell command blocked by policy" }
	end

	return nil
end, { once = false })

local handle = mcode.timer.every(1000, function()
	local snapshot = mcode.session.snapshot()
	mcode.log.debug("seq", snapshot.seq)
end)
handle:stop()

local ok, body = mcode.fs.read("README.md")

-- fs.read returns (value, err), so the value is optional until checked.
if ok and body then
	mcode.emit("custom.event", { length = #body })
end

local response = mcode.net.get("https://example.com", { timeout_ms = 2000 })

if response then
	mcode.log.info(response.status)
end

mcode.context.add_instructions("Prefer the repository's existing style.", { priority = 10 })
mcode.skill.register({ name = "example", description = "d", body = "b" })
mcode.mcp.register({ name = "files", transport = "stdio", command = { "server" } })

local util = require("./lib/util")
print(util.name)

mcode.tool.unregister(tool_id)
