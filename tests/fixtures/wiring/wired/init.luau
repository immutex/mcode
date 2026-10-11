--!strict
-- Proves each gated API entry reaches its collaborator. A surface installed
-- without one returns nil, and the assertions in test_api_wiring.cxx fail.

local mcode = mcode

mcode.cmd.register("wired-cmd", function(args)
	return "ran with: " .. tostring(args)
end, { description = "A command an extension registered" })

mcode.defer(function()
	mcode.notify("deferred ran", "info")
end)

mcode.context.add_instructions("the wired fixture contributed this")

local snapshot, snapshot_err = mcode.session.snapshot()

if snapshot ~= nil then
	mcode.notify("session " .. snapshot.session .. " seq " .. tostring(snapshot.seq), "info")
else
	mcode.notify("snapshot failed: " .. tostring(snapshot_err), "error")
end
