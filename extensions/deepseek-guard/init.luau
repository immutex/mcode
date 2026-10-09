--!strict
-- Detects DeepSeek's own tool-call markup leaking into assistant text.
--
-- This is the one failure mode of the three this extension was written for that
-- the harness does NOT already cover, and the live session is what showed that:
--
--   * Reasoning that repeats itself -- the core detects it in
--     `agent/repetition_guard.hxx` and RE-STEERS the model. Reimplementing it
--     here would double the warnings and split one rule across two places.
--   * A tool called repeatedly with identical arguments -- the core's
--     `thrash_detector` refuses the third consecutive dispatch. Same reasoning.
--
-- So what is left is the leak. The model's tool-call wrapper -- `<|DSML|...|>`
-- and friends -- sometimes arrives inside the content stream instead of being
-- parsed into tool calls. It is reported against several serving paths (vLLM,
-- NVIDIA's Spark builds) and several harnesses, which makes it a gateway
-- problem rather than a model-weight one: a harness cannot repair it.
--
-- What a harness CAN do is refuse to let it pass as an answer. Markup in the
-- assistant text is not prose, and a model that has been shown it may imitate
-- it. This says so, once per turn, and counts the occurrences.
--
-- It only notifies. It does not rewrite the text, retry the request, or veto
-- anything: the harness's job at this layer is to make the failure visible
-- rather than to paper over it, and a guard that silently stripped the markup
-- would hide a gateway regression instead of reporting it.
--
-- No permissions: it reads no files, makes no network calls, spawns nothing.

-- The wrappers the DeepSeek serving path is known to emit around a tool call.
-- Both the full-width pipe form and the ASCII fallback are matched, because
-- different builds and gateways have emitted either.
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

	-- Once per turn. A leaked wrapper arrives across many deltas, and a
	-- notification per delta would be the failure rather than the report.
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
