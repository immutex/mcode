#pragma once

#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::tools {

	// Deterministic repair of a JSON value a model produced for tool arguments: recovers a
	// markdown fence, surrounding prose, a trailing comma, single-quoted strings, Python
	// literals, and a tail cut mid-token, and never invents a value.
	[[nodiscard]] auto repair_json( std::string_view text ) -> result< std::string >;

}
