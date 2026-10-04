#include "mcode/ext/lua_host.hxx"
#include "mcode/ext/lua_host_internal.hxx"

#include <chrono>
#include <cstdint>
#include <string>

#include "lua.h"
#include "lualib.h"

namespace mcode::ext::detail {

	auto interrupt( lua_State* state, int gc ) -> void {
		// gc >= 0 is a GC step, which shares this callback.
		if ( gc >= 0 ) {
			return;
		}

		static thread_local std::uint64_t g_safepoints = 0;

		if ( ++g_safepoints % ext::detail::INTERRUPT_GRANULARITY != 0 ) {
			return;
		}

		// the deadline is per-thread, so one extension's budget cannot fire inside another.
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
			lua_pushnil( state );
			lua_pushlstring( state, produced.error( ).msg.data( ), produced.error( ).msg.size( ) );

			return 2;
		}

		lua_pushlstring( state, produced->data( ), produced->size( ) );
		lua_pushnil( state );

		return 2;
	}

}
