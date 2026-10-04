#pragma once

// the policy lives in api_gate: one `net:<host>` declaration gates every outbound host.

#include <string>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_net_get( lua_State* state ) -> int;
	auto handle_net_search( lua_State* state ) -> int;

}
