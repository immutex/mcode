#pragma once

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_task( lua_State* state ) -> int;

}
