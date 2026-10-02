#pragma once

// an API-level allowlist, not the process-level egress proxy; only manifest-declared hosts.

#include <string>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_net_get( lua_State* state ) -> int;
	auto handle_net_search( lua_State* state ) -> int;

}
