--!strict
-- Reference providers, registered as data (docs/26 decision 2).
--
-- This file is the dogfood test for the provider descriptor: if a real provider
-- cannot be expressed here, the descriptor is wrong. Three wire formats are
-- covered, one per shape docs/15 identifies.
--
-- An extension never sees a token. HTTP, SSE framing, retry, the error taxonomy,
-- and egress policy stay in C++; this file supplies only the mapping from each
-- provider's JSON to the canonical event model, and C++ applies it natively.

mcode.model.register({
	name = "openai-chat-completions",
	endpoint = "https://api.openai.com/v1/chat/completions",
	auth = { from = "env", name = "OPENAI_API_KEY", header = "Authorization" },

	-- [OI] Chat Completions: stateless `messages`, tool calls as
	-- choices[0].delta.tool_calls[] fragments keyed by index.
	stream = {
		text_delta = "/choices/0/delta/content",
		thinking_delta = "/choices/0/delta/reasoning_content",
		tool_calls = {
			index = "/choices/0/delta/tool_calls/0/index",
			id = "/choices/0/delta/tool_calls/0/id",
			name = "/choices/0/delta/tool_calls/0/function/name",
			args = "/choices/0/delta/tool_calls/0/function/arguments",
		},
		finish = "/choices/0/finish_reason",
		usage = {
			-- `in` is a reserved word in Luau, so the key is bracketed here and in
			-- mcode.d.luau. A descriptor written as JSON uses the plain "in" key,
			-- and both forms map to the same field.
			["in"] = "/usage/prompt_tokens",
			out = "/usage/completion_tokens",
			cached_read = "/usage/prompt_tokens_details/cached_tokens",
		},
	},
})

mcode.model.register({
	name = "openai-responses",
	endpoint = "https://api.openai.com/v1/responses",
	auth = { from = "env", name = "OPENAI_API_KEY", header = "Authorization" },

	-- [OI] Responses: semantic events, and the event NAME carries the meaning as
	-- much as the payload does. Text arrives as response.output_text.delta and
	-- tool arguments as response.function_call_arguments.delta -- the same
	-- pointer on both, distinguished by the event name.
	stream = {
		text_delta = "/delta",
		text_events = { "response.output_text.delta" },

		tool_calls = {
			id = "/item_id",
			args = "/delta",
		},
		tool_call_events = { "response.function_call_arguments.delta" },

		terminal_events = { "response.completed", "response.failed", "response.incomplete" },
	},
})

mcode.model.register({
	name = "anthropic-messages",
	endpoint = "https://api.anthropic.com/v1/messages",
	auth = { from = "env", name = "ANTHROPIC_API_KEY", header = "x-api-key", scheme = "" },

	-- Anthropic: content_block_start / content_block_delta / content_block_stop
	-- around each block, with thinking as a separate delta type. Usage arrives
	-- cumulatively in message_delta, which is why the applier takes the max.
	stream = {
		text_delta = "/delta/text",
		thinking_delta = "/delta/thinking",
		tool_calls = {
			index = "/index",
			id = "/content_block/id",
			name = "/content_block/name",
			args = "/delta/partial_json",
		},
		finish = "/delta/stop_reason",
		usage = {
			["in"] = "/message/usage/input_tokens",
			out = "/usage/output_tokens",
			cached_read = "/message/usage/cache_read_input_tokens",
			cache_write = "/message/usage/cache_creation_input_tokens",
		},
		terminal_events = { "message_stop" },
	},
})
