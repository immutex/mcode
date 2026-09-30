#include "mcode/ext/defer.hxx"

#include <string>
#include <utility>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		// The deferred queue is bounded. An extension that defers in a loop hits
		// the cap and gets an error, the same answer an unbounded timer registry
		// would give.
		inline constexpr auto MAX_DEFERRED = std::size_t{ 256 };

	}

	auto handle_defer( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TFUNCTION ) {
			lua_pushliteral( state, "mcode.defer expects a function" );
			lua_error( state );
		}

		auto* queue = self->deferred( );

		if ( queue == nullptr ) {
			lua_pushliteral( state, "mcode.defer: no deferred queue was installed with "
				"this surface" );
			lua_error( state );
		}

		if ( queue->size( ) >= MAX_DEFERRED ) {
			lua_pushliteral( state, "mcode.defer: the deferred queue is full" );
			lua_error( state );
		}

		lua_pushvalue( state, 1 );

		const auto reference = lua_ref( state, -1 );
		lua_pop( state, 1 );

		queue->push_back( reference );

		return 0;
	}

}
