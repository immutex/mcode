#pragma once

#include "lua.h"

namespace mcode::ext {

	// takes a query, never a path: the host resolves it against the workspace ledger,
	// so the entry point cannot become an arbitrary-file read.
	auto handle_memory_search( lua_State* state ) -> int;

}
