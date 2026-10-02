#pragma once

#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

struct lua_State;

namespace mcode::ext {

	// json null becomes nil: an object member is then absent, an array element a hole.
	[[nodiscard]] auto push_json( lua_State* state, std::string_view text ) -> status;

	// functions, userdata, threads and cycles are refused, not silently skipped.
	[[nodiscard]] auto json_from_lua( lua_State* state, int index ) -> result< std::string >;

}
