#pragma once

#include <string>
#include <vector>

#include "mcode/ext/api.hxx"

#include "lua.h"

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

	// True when argument `index` was not passed at all.
	//
	// `lua_type` reports LUA_TNONE (-1) for a missing argument, not LUA_TNIL
	// (0), so a guard written as `lua_type( state, 2 ) != LUA_TNIL` treats an
	// OMITTED argument as a wrong-typed one and rejects the documented
	// one-argument call form. Every optional argument needs this instead.
	[[nodiscard]] inline auto argument_absent( lua_State* state, const int index ) -> bool {
		const auto type = lua_type( state, index );

		return type == LUA_TNONE || type == LUA_TNIL;
	}

}
