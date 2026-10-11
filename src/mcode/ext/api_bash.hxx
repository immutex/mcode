#pragma once

struct lua_State;

namespace mcode::ext {

	auto handle_bash( lua_State* state ) -> int;

}
