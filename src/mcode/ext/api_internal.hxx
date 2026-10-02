#pragma once

#include <string>
#include <vector>

#include "mcode/ext/api.hxx"

struct lua_State;

namespace mcode::ext {

	// a missing upvalue is a contract violation, not a null return.
	[[nodiscard]] auto surface_from( lua_State* state ) -> api_surface*;

	// an absent or non-string field is the empty string; the caller decides if that is an error.
	[[nodiscard]] auto read_field_string( lua_State* state, int table, const char* key )
		-> std::string;

	// non-string elements are skipped rather than coerced.
	[[nodiscard]] auto read_field_string_array( lua_State* state, int table, const char* key )
		-> std::vector< std::string >;

}
