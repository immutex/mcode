#pragma once

// always available: presentation, not capability; attribution is unforgeable.

#include <string>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_notify( lua_State* state ) -> int;

}
