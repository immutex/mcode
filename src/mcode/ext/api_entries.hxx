#pragma once

#include <span>

#include "lua.h"

namespace mcode::ext {

	// One entry in the `mcode` table an extension sees.
	struct api_entry {
		const char* path;
		lua_CFunction function;
	};

	// The whole surface, in one table. A `lua_CFunction` cannot be a member pointer,
	// so each entry needs a free function forwarding to the surface it is bound to.
	// A name in the type stub but absent here is the defect the surface test
	// exists to catch.
	[[nodiscard]] auto api_entries( ) -> std::span< const api_entry >;

}
