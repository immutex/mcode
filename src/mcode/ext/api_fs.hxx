#pragma once

// same handlers as the model's own file tools, so the workspace boundary cannot drift.

struct lua_State;

namespace mcode::ext {

	class api_surface;

	auto handle_fs_read( lua_State* state ) -> int;
	auto handle_fs_write( lua_State* state ) -> int;

}
