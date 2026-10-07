#pragma once

#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::tools {

	// Deterministic repair of a JSON value a model produced for tool arguments. Recovers only
	// the shapes observed in practice - a markdown fence, surrounding prose, a trailing comma,
	// single-quoted strings, Python literals, and a tail cut off mid-token - and never invents
	// a value that was not in the input. A payload with nothing to recover is an error.
	[[nodiscard]] auto repair_json( std::string_view text ) -> result< std::string >;

}
