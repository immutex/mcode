#pragma once

// requires the `context` permission; over-budget contributions are truncated, not dropped.

#include <string>

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_context_add_instructions( lua_State* state ) -> int;

}
