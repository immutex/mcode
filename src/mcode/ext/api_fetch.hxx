#pragma once

struct lua_State;

namespace mcode::ext {

	auto handle_fetch( lua_State* state ) -> int;

}
