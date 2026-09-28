#pragma once

#include <string>

#include "mcode/model/client.hxx"

namespace mcode::model {

	// Renders the [OI] Chat Completions request shape: `messages` as role and
	// content strings, tools as `function{name,description,parameters}`, and
	// top-level `max_tokens`/`temperature`/`stream`/`response_format`.
	//
	// Byte-stable by construction: the body is built through the json setters,
	// whose container holds members in sorted key order, and tool schemas are
	// sorted by name before rendering. No timestamps, no cwd.
	//
	// Cache breakpoints are byte offsets into the rendered string, applied
	// right to left so each offset is still valid when it is used.
	[[nodiscard]] auto render_chat_completions( const chat_request& request,
		const request_spec& fields ) -> result< std::string >;

	// True when the descriptor's wire shape is the one this branch renders.
	// `request_spec` carries field names but no field shapes, so an Anthropic
	// body (top-level system, content as block arrays) is not renderable from a
	// descriptor today; the refusal is by name rather than a malformed request.
	[[nodiscard]] auto is_renderable_shape( const provider_descriptor& descriptor ) noexcept -> bool;

}
