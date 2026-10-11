#pragma once

// drained on the loop thread; a deferred closure dies with its VM.

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_defer( lua_State* state ) -> int;

}
