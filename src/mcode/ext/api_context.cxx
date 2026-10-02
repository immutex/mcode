#include "mcode/ext/api_context.hxx"

#include <string>
#include <utility>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_gate.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		inline constexpr auto MAX_CONTRIBUTION_CHARS = std::size_t{ 8'192 };

	}

	auto handle_context_add_instructions( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.context.add_instructions expects the text "
				"as a string" );
			lua_error( state );
		}

		if ( lua_type( state, 2 ) != LUA_TNIL && lua_type( state, 2 ) != LUA_TTABLE ) {
			lua_pushliteral( state, "mcode.context.add_instructions: opts must be a "
				"table or nil" );
			lua_error( state );
		}

		if ( !manifest_allows( *self, "context" ) ) {
			return deny_permission( state, "context",
				"mcode.context.add_instructions" );
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );
		auto contribution = std::string{ text != nullptr ? text : "", length };

		auto truncated = false;

		if ( contribution.size( ) > MAX_CONTRIBUTION_CHARS ) {
			contribution.resize( MAX_CONTRIBUTION_CHARS );
			truncated = true;
		}

		auto* sink = self->instruction_sink( );

		if ( sink == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no instruction sink was installed with this "
				"surface" );

			return 2;
		}

		( *sink )( self->manifest( ).name, contribution );

		if ( truncated ) {
			lua_pushboolean( state, 0 );
			lua_pushliteral( state, "the contribution was truncated at the "
				"per-extension cap" );

			return 2;
		}

		lua_pushboolean( state, 1 );

		return 1;
	}

}
