#pragma once

#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

struct lua_State;

namespace mcode::ext {

	// Decodes one JSON value and pushes it onto the stack.
	//
	// The host marshals structured data as JSON because the alternative is a
	// per-call C++ type that every extension has to match. JSON is the wire format
	// the model already speaks, so a tool argument table is the same shape whether
	// it came from the model or from a test.
	//
	// Objects become tables, arrays become sequences, and a JSON null becomes
	// `nil` -- which means an object member that is null is ABSENT from the table
	// rather than present-and-nil. Lua cannot distinguish the two, and pretending
	// otherwise would produce a table that looks empty.
	//
	// The same rule applies INSIDE an array, and its consequence is worth stating
	// because it is not obvious: `[1, null, 3]` becomes a table with a hole at
	// index 2. `ipairs` stops at the hole and `#` is undefined for it, so an
	// extension that expects a fixed-length sequence must check the indices it
	// needs rather than trusting the length. A sentinel would avoid this at the
	// cost of a value the model cannot express, which is why nil is the choice.
	[[nodiscard]] auto push_json( lua_State* state, std::string_view text ) -> status;

	// Encodes the value at `index` as JSON, leaving the stack unchanged.
	//
	// Functions, userdata, threads, and cycles are refused rather than skipped: a
	// table that cannot be represented is a bug in the extension, and silently
	// dropping a field would hand the model a payload that does not match what the
	// extension produced.
	[[nodiscard]] auto json_from_lua( lua_State* state, int index ) -> result< std::string >;

}
