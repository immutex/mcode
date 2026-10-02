#pragma once

// read-only and scoped: an extension reads only its own `[extensions.<name>]` section.

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_cfg_get( lua_State* state ) -> int;

}
