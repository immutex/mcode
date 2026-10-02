#pragma once

#include <string>

#include "mcode/model/client.hxx"

namespace mcode::model {

	// byte-stable: sorted keys and tool schemas, no timestamps, so the prompt cache hits.
	[[nodiscard]] auto render_chat_completions( const chat_request& request,
		const request_spec& fields ) -> result< std::string >;

	// `request_spec` carries field names but no field shapes, so the refusal is by name.
	[[nodiscard]] auto is_renderable_shape( const provider_descriptor& descriptor ) noexcept -> bool;

}
