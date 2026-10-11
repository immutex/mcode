#pragma once

#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::mcp::detail {

	// Flattens the protocol's content-part array to its text: a part with no `text`
	// member (an image, an embedded resource) contributes nothing rather than a marker,
	// because a marker would be indistinguishable from the server saying it.
	[[nodiscard]] auto flatten_content( std::string_view result_json )
		-> result< std::string >;

	// The first `mimeType` in a content-part array, or empty when the server omitted it.
	[[nodiscard]] auto content_mime_type( std::string_view result_json ) -> std::string;

	// A JSON string literal, quotes included.
	[[nodiscard]] auto json_escape( std::string_view text ) -> std::string;

}
