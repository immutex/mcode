--!strict
-- Reports DeepSeek tool-call markup leaking into assistant text. It only
-- notifies: rewriting or vetoing would hide a gateway fault rather than surface it.

-- Both the full-width pipe form and the ASCII fallback: different builds and
-- gateways have emitted either.
local MARKERS = {
	"\u{FF5C}DSML\u{FF5C}",
	"<|DSML|",
	"|DSML|>",
	"<tool_calls>",
	"</tool_calls>",
}

local leaks = 0
local reported_this_turn = false

local function first_marker( text: string ): string?
	for _, marker in MARKERS do
		if text:find(marker, 1, true) then
			return marker
		end
	end

	return nil
end

mcode.on("turn.start", function( _ )
	reported_this_turn = false

	return nil
end)

mcode.on("assistant.delta", function( event )
	local text = event.payload.text

	if type(text) ~= "string" or text == "" then
		return nil
	end

	local marker = first_marker(text)

	if marker == nil then
		return nil
	end

	leaks += 1

	-- Once per turn: a leaked wrapper spans many deltas, so a notification per
	-- delta would be the failure rather than the report.
	if not reported_this_turn then
		reported_this_turn = true

		mcode.notify(string.format(
			"the model's tool-call markup (%s) leaked into its answer text. This is a "
			.. "gateway fault, not something the harness can repair -- treat the "
			.. "surrounding text as unreliable. Leaks this session: %d",
			marker, leaks), "warn")
	end

	return nil
end)

mcode.on("turn.end", function( _ )
	if leaks > 0 then
		mcode.log.info(string.format("markup_leaks=%d", leaks))
	end

	return nil
end)
