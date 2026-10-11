#include "mcode/ext/api_memory.hxx"

#include <cstddef>
#include <string>
#include <string_view>

#include "lua.h"

#include "mcode/ext/api_gate.hxx"
#include "mcode/ext/api_internal.hxx"
#include "mcode/memory/ledger.hxx"
#include "mcode/support/text.hxx"

namespace mcode::ext {

	namespace {

		// The same cap the search tools use: enough to answer, not enough to flood
		// the window. The caller pages rather than receiving everything.
		constexpr std::size_t MAX_MATCHES = 50;
		constexpr std::size_t MAX_MATCH_LINES = 200;

	}

	auto handle_memory_search( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.memory.search expects a query string" );
			lua_error( state );
		}

		if ( !manifest_allows( *self, "fs_read" ) ) {
			return deny_permission( state, "fs_read", "mcode.memory.search" );
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );
		const auto query = mcode::text::ascii_lower(
			std::string_view{ text != nullptr ? text : "", length } );

		if ( query.empty( ) ) {
			lua_pushnil( state );
			lua_pushliteral( state, "a query is required" );

			return 2;
		}

		const auto* context = self->file_context( );

		if ( context == nullptr || context->space == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no workspace was installed with this surface" );

			return 2;
		}

		auto ledger = mcode::memory::read_ledger( context->space->root( ) );

		if ( !ledger ) {
			lua_pushnil( state );
			lua_pushlstring( state, ledger.error( ).msg.data( ), ledger.error( ).msg.size( ) );

			return 2;
		}

		if ( ledger->empty( ) ) {
			lua_pushliteral( state, "no notes recorded for this project" );

			return 1;
		}

		auto out = std::string{ };
		auto matches = std::size_t{ 0 };
		auto examined = std::size_t{ 0 };
		auto line_start = std::size_t{ 0 };

		while ( line_start < ledger->size( ) && examined < MAX_MATCH_LINES ) {
			const auto line_end = ledger->find( '\n', line_start );
			const auto stop = line_end == std::string::npos ? ledger->size( ) : line_end;
			const auto line = std::string_view{ *ledger }.substr( line_start, stop - line_start );

			++examined;

			if ( !line.empty( )
				&& mcode::text::ascii_lower( line ).find( query ) != std::string::npos ) {
				out += line;
				out += '\n';
				++matches;

				if ( matches >= MAX_MATCHES ) {
					break;
				}
			}

			if ( line_end == std::string::npos ) {
				break;
			}

			line_start = line_end + 1;
		}

		if ( matches == 0 ) {
			lua_pushliteral( state, "no notes matched" );

			return 1;
		}

		lua_pushlstring( state, out.data( ), out.size( ) );

		return 1;
	}

}
