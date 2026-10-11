#pragma once

// presentation, not capability: always available, and the attribution is unforgeable.

#include <string>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_notify( lua_State* state ) -> int;

}
