#include "mcode/ext/api.hxx"

#include <algorithm>
#include <string>
#include <utility>

#include "lua.h"

#include "mcode/support/mcp_config.hxx"

namespace mcode::ext {

	namespace {

		// Same retrieval as api.cxx's helper: the upvalue is the owner surface.
		// Duplicated because the original lives in that file's anonymous
		// namespace and the closure contract is one lightuserdata at index 1.
		auto surface_from( lua_State* state ) -> api_surface* {
			auto* surface = static_cast< api_surface* >(
				lua_touserdata( state, lua_upvalueindex( 1 ) ) );

			if ( surface == nullptr ) {
				lua_pushliteral( state, "host API is not bound" );
				lua_error( state );
			}

			return surface;
		}

		auto read_field_string( lua_State* state, const int table, const char* key )
			-> std::string {
			lua_getfield( state, table, key );

			auto value = std::string{ };

			if ( lua_type( state, -1 ) == LUA_TSTRING ) {
				auto length = std::size_t{ 0 };
				const auto* text = lua_tolstring( state, -1, &length );

				if ( text != nullptr ) {
					value.assign( text, length );
				}
			}

			lua_pop( state, 1 );

			return value;
		}

		auto read_field_string_array( lua_State* state, const int table, const char* key )
			-> std::vector< std::string > {
			lua_getfield( state, table, key );

			auto out = std::vector< std::string >{ };

			if ( lua_type( state, -1 ) == LUA_TTABLE ) {
				const auto items = lua_absindex( state, -1 );
				const auto length = static_cast< std::size_t >( lua_objlen( state, items ) );

				out.reserve( length );

				for ( auto index = std::size_t{ 1 }; index <= length; ++index ) {
					lua_rawgeti( state, items, static_cast< int >( index ) );

					if ( lua_type( state, -1 ) == LUA_TSTRING ) {
						auto length_bytes = std::size_t{ 0 };
						const auto* text = lua_tolstring( state, -1, &length_bytes );

						if ( text != nullptr ) {
							out.emplace_back( text, length_bytes );
						}
					}

					lua_pop( state, 1 );
				}
			}

			lua_pop( state, 1 );

			return out;
		}

	}

	auto api_surface::handle_mcp_register( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TTABLE ) {
			lua_pushliteral( state, "mcode.mcp.register expects a definition table" );
			lua_error( state );
		}

		// The `mcp` permission gates the call. A denied call is environmental
		// failure, not a contract violation, so it returns `nil, err` -- the same
		// channel as `fs.read` on a missing file.
		if ( !self->manifest_.has_permission( "mcp" ) ) {
			lua_pushnil( state );
			lua_pushliteral( state, "permission denied: 'mcp' is required to declare a "
				"server" );

			return 2;
		}

		const auto definition = lua_absindex( state, 1 );

		auto server = mcode::mcp::server_config{ };
		server.name = read_field_string( state, definition, "name" );
		server.transport = read_field_string( state, definition, "transport" );
		server.command = read_field_string_array( state, definition, "command" );
		server.tools = read_field_string_array( state, definition, "tools" );

		// The declaration route and the config route produce the same struct:
		// one validation, one consumer. An extension-declared server is not
		// already approved -- `enabled` stays false and the launch command still
		// requires explicit consent the first time it runs.
		server.source = mcode::mcp::server_source::extension;
		server.enabled = false;

		if ( auto validated = mcode::mcp::validate_server( server ); !validated ) {
			lua_pushnil( state );
			lua_pushlstring( state, validated.error( ).msg.data( ),
				validated.error( ).msg.size( ) );

			return 2;
		}

		if ( self->servers_ == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no server store was installed with this surface" );

			return 2;
		}

		if ( auto stored = self->servers_->add( std::move( server ) ); !stored ) {
			lua_pushnil( state );
			lua_pushlstring( state, stored.error( ).msg.data( ),
				stored.error( ).msg.size( ) );

			return 2;
		}

		lua_pushboolean( state, 1 );

		return 1;
	}

}
