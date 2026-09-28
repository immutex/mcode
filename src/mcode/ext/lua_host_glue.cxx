#include "mcode/ext/lua_host.hxx"
#include "mcode/ext/lua_host_internal.hxx"

#include <chrono>
#include <cstdint>
#include <string>

#include "lua.h"
#include "lualib.h"

namespace mcode::ext::detail {

	auto interrupt( lua_State* state, int gc ) -> void {
		// Non-negative values are GC steps, which share this callback.
		if ( gc >= 0 ) {
			return;
		}

		static thread_local std::uint64_t safepoints = 0;

		if ( ++safepoints % ext::detail::INTERRUPT_GRANULARITY != 0 ) {
			return;
		}

		// The callback is global state but the deadline is not: this looks the
		// watchdog up through the running thread, so extension A's budget can
		// never fire inside extension B.
		auto* watchdog = ext::detail::watchdog_from( state );

		if ( watchdog == nullptr || !watchdog->armed ) {
			return;
		}

		if ( std::chrono::steady_clock::now( ) < watchdog->deadline ) {
			return;
		}

		watchdog->expired = true;
		++watchdog->breaches;

		luaL_error( state, "extension exceeded its time budget" );
	}


	auto host_function_dispatch( lua_State* state ) -> int {
		auto* function = static_cast< host_function* >( lua_touserdata( state, lua_upvalueindex( 1 ) ) );

		if ( function == nullptr || !*function ) {
			lua_pushliteral( state, "host function is not bound" );

			lua_error( state );
		}

		auto args = std::string{ };

		if ( lua_gettop( state ) >= 1 && lua_isstring( state, 1 ) != 0 ) {
			auto length = std::size_t{ 0 };
			const auto* text = lua_tolstring( state, 1, &length );

			if ( text != nullptr ) {
				args.assign( text, length );
			}
		}

		auto produced = ( *function )( args );

		if ( !produced ) {
			// `nil, err` is the environmental-failure channel: the model gets a
			// message it can act on and the harness does not treat it as a crash.
			lua_pushnil( state );
			lua_pushlstring( state, produced.error( ).msg.data( ), produced.error( ).msg.size( ) );

			return 2;
		}

		lua_pushlstring( state, produced->data( ), produced->size( ) );
		lua_pushnil( state );

		return 2;
}
}
