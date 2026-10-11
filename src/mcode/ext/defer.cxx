#include "mcode/ext/defer.hxx"

#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"
#include "mcode/ext/lua_host.hxx"

namespace mcode::ext {

	namespace {

		inline constexpr auto MAX_DEFERRED = std::size_t{ 256 };

	}

	auto handle_defer( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TFUNCTION ) {
			lua_pushliteral( state, "mcode.defer expects a function" );
			lua_error( state );
		}

		auto& queue = self->deferred( );

		if ( queue.size( ) >= MAX_DEFERRED ) {
			lua_pushliteral( state, "mcode.defer: the deferred queue is full" );
			lua_error( state );
		}

		lua_pushvalue( state, 1 );

		const auto reference = lua_ref( state, -1 );
		lua_pop( state, 1 );

		queue.push_back( reference );

		return 0;
	}

	auto api_surface::pump_deferred( ) -> void {
		if ( deferred_.empty( ) ) {
			return;
		}

		auto* state = host_ != nullptr ? host_->raw( ) : nullptr;

		if ( state == nullptr ) {
			deferred_.clear( );

			return;
		}

		// Swapped out first: a callback that defers again queues for the next pump
		// instead of extending this one without bound.
		auto due = std::vector< int >{ };
		due.swap( deferred_ );

		for ( const auto reference : due ) {
			// budgets the call: a deferred callback must not run forever on the loop's thread.
			auto budget = lua_host::budget_scope{ host_ };

			const auto depth = lua_gettop( state );

			lua_getref( state, reference );

			if ( lua_type( state, -1 ) == LUA_TFUNCTION ) {
				// A raising callback is its own report; the pump must not end the run.
				std::ignore = lua_pcall( state, 0, 0, 0 );
			}

			lua_settop( state, depth );
			lua_unref( state, reference );
		}
	}

}
