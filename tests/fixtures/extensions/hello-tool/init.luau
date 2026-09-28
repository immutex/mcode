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

mcode.log.info("loaded", mcode.ext.name)
