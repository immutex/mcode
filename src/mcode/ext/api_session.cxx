#include "mcode/ext/api_session.hxx"

#include "lua.h"
#include "lualib.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_gate.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	auto handle_session_snapshot( lua_State* state ) -> int {
		auto* self = surface_from( state );

		const auto* provider = self->session_state( );

		if ( provider == nullptr ) {
			// No session context was installed with the surface. A snapshot of
			// nothing is an environmental failure, not an empty success: an
			// extension that believes it saw state it did not is worse than one
			// that must handle `nil`.
			lua_pushnil( state );
			lua_pushliteral( state, "no session context was installed with this "
				"surface" );

			return 2;
		}

		const auto current = ( *provider )( );

		lua_createtable( state, 0, 4 );

		lua_pushlstring( state, current.session.data( ), current.session.size( ) );
		lua_setfield( state, -2, "session" );

		lua_pushnumber( state, static_cast< double >( current.seq ) );
		lua_setfield( state, -2, "seq" );

		lua_pushnumber( state, static_cast< double >( current.steps ) );
		lua_setfield( state, -2, "steps" );

		if ( current.has_goal ) {
			lua_pushlstring( state, current.goal.data( ), current.goal.size( ) );
		} else {
			lua_pushnil( state );
		}

		lua_setfield( state, -2, "goal" );

		return 1;
	}

	auto handle_session_fork( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TNUMBER ) {
			lua_pushliteral( state, "mcode.session.fork expects the sequence to branch "
				"at as a number" );
			lua_error( state );
		}

		if ( lua_type( state, 2 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.session.fork expects a label string" );
			lua_error( state );
		}

		if ( !manifest_allows( *self, "session_fork" ) ) {
			return deny_permission( state, "session_fork", "mcode.session.fork" );
		}

		const auto at_seq = static_cast< std::uint64_t >( luaL_checknumber( state, 1 ) );

		auto length = std::size_t{ 0 };
		const auto* label_text = lua_tolstring( state, 2, &length );
		const auto label = std::string{ label_text != nullptr ? label_text : "", length };

		auto* forker = self->session_forker( );

		if ( forker == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no session context was installed with this "
				"surface" );

			return 2;
		}

		const auto forked = ( *forker )( at_seq, label );

		if ( !forked ) {
			lua_pushnil( state );
			lua_pushlstring( state, forked.error( ).msg.data( ),
				forked.error( ).msg.size( ) );

			return 2;
		}

		lua_pushlstring( state, forked->session.data( ), forked->session.size( ) );

		return 1;
	}

}
