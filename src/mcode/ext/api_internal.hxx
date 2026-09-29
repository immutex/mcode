#pragma once

#include <string>
#include <vector>

#include "mcode/ext/api.hxx"

struct lua_State;

namespace mcode::ext {

	// Helpers shared by the files that install the frozen surface: `api.cxx`
	// holds the original set and `api_skill.cxx` / `api_mcp.cxx` each add a
	// namespace. They were copied per file, with a comment admitting it; three
	// copies of the upvalue contract is three places for it to drift, and the
	// upvalue contract is the one thing every entry point depends on.

	// The owner surface, from the closure's single upvalue. A missing binding is
	// a contract violation and raises rather than returning null, because every
	// caller would otherwise have to re-check it.
	[[nodiscard]] auto surface_from( lua_State* state ) -> api_surface*;

	// A string field of the table at `table`. A missing or non-string field is
	// the empty string; the caller decides whether that is an error, because
	// "absent" and "wrong type" differ per entry point.
	[[nodiscard]] auto read_field_string( lua_State* state, int table, const char* key )
		-> std::string;

	// An array-of-strings field. Non-string elements are skipped rather than
	// coerced, so a malformed array cannot silently become a partial one.
	[[nodiscard]] auto read_field_string_array( lua_State* state, int table, const char* key )
		-> std::vector< std::string >;

}
