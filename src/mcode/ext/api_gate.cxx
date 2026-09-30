#include "mcode/ext/api_gate.hxx"

#include "lua.h"

#include "mcode/ext/api.hxx"

namespace mcode::ext {

	auto deny_permission( lua_State* state, const char* permission, const char* entry )
		-> int {
		lua_pushnil( state );

		lua_pushliteral( state, "permission denied: '" );
		lua_pushstring( state, permission );
		lua_pushliteral( state, "' is required for " );
		lua_pushstring( state, entry );

		lua_concat( state, 4 );

		return 2;
	}

	auto manifest_allows( const api_surface& surface, const char* permission ) -> bool {
		return surface.manifest( ).has_permission( permission );
	}

}
