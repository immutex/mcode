#pragma once

#include "lua.h"

namespace mcode::ext {

	// `mcode.memory.search(query)` -- greps the user-authored notes ledger.
	//
	// Takes a QUERY, never a path: the host resolves it against the one ledger for
	// the workspace, so the entry point cannot be turned into an arbitrary-file
	// read. Structurally the same as `mcode.skill.read`.
	auto handle_memory_search( lua_State* state ) -> int;

}
