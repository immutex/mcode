#pragma once

struct lua_State;

namespace mcode::ext {

	auto handle_plan_set( lua_State* state ) -> int;

}
