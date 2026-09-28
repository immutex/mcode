--!strict
-- The dogfood extension: real capability, shipped through the same surface a
-- third-party author gets. If this cannot be written, the API is wrong.

local mcode = mcode

mcode.tool.register({
	name = "hello",
	description = "Greet the named file's owner",
	schema = { type = "object", properties = { name = { type = "string" } } },
	run = function(args, ctx)
		-- The tool's whole point is the round trip: the model's arguments arrive
		-- as a table, the extension reads a real file, and the answer goes back
		-- through the registry. No C++ knows this tool exists.
		local target = args.name

		-- An environmental failure returns `nil, err` (docs/18): the model gets a
		-- message it can act on, and the harness does not treat it as a crash.
		if target == nil then
			return nil, "name is required"
		end

		return "hello from " .. ctx.extension .. " to " .. target, nil
	end,
})

-- A hook, in the extension language: docs/26's exit criterion names a provider, a
-- tool, AND a hook as the three things that must be implementable in Luau.
mcode.on("tool.pre_call", function(ev)
	if ev.payload and ev.payload.name == "forbidden" then
		return { veto = "blocked by hello-tool" }
	end

	return nil
end)

-- And a custom event, which never reaches the session log: docs/20 keeps the log's
-- schema closed, so an extension-invented name must not become a kind.
mcode.on("hello-tool.ready", function(ev)
	mcode.log.info("ready event seen", ev.payload.count)

	-- An explicit nil: the declared return is `{ veto: string }?`, and --!strict
	-- requires every codepath to return it.
	return nil
end)

mcode.log.info("loaded", mcode.ext.name)
