#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "mcode/model/client.hxx"

namespace mcode::model {

	// byte-stable: sorted keys and tool schemas, no timestamps, so the prompt cache hits.
	[[nodiscard]] auto render_chat_completions( const chat_request& request,
		const request_spec& fields ) -> result< std::string >;

	// `request_spec` carries field names but no field shapes, so the refusal is by name.
	[[nodiscard]] auto is_renderable_shape( const provider_descriptor& descriptor ) noexcept -> bool;

	// The byte offsets of the stable-prefix cache breakpoint: just inside the first message
	// object, so the marker covers the system message and the tools block before it. These are
	// positions in the rendered body, so the anchor is located by rendering; empty when the
	// request has no messages or already carries breakpoints of its own.
	[[nodiscard]] auto cache_breakpoints( const chat_request& request,
		const request_spec& fields ) -> std::vector< std::size_t >;

}
