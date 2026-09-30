#pragma once

// `mcode.net.get` and `mcode.net.search`.
//
// An API-level host allowlist, NOT the M6 egress proxy: the process-level
// egress control is a different mechanism and this is a policy on what
// extension code may ASK for. An extension may reach only the hosts its
// manifest declares. Requires the `net` permission, checked against the
// calling extension's own manifest.

#include <string>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_net_get( lua_State* state ) -> int;
	auto handle_net_search( lua_State* state ) -> int;

}
