#include "mcode/ext/api_internal.hxx"

#include "lua.h"

namespace mcode::ext {

	auto surface_from( lua_State* state ) -> api_surface* {
		auto* surface = static_cast< api_surface* >(
			lua_touserdata( state, lua_upvalueindex( 1 ) ) );

		if ( surface == nullptr ) {
			lua_pushliteral( state, "host API is not bound" );
			lua_error( state );
		}

		return surface;
	}

	auto read_field_string( lua_State* state, const int table, const char* key ) -> std::string {
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
			const auto count = static_cast< std::size_t >( lua_objlen( state, items ) );

			out.reserve( count );

			for ( auto index = std::size_t{ 1 }; index <= count; ++index ) {
				lua_rawgeti( state, items, static_cast< int >( index ) );

				if ( lua_type( state, -1 ) == LUA_TSTRING ) {
					auto length = std::size_t{ 0 };
					const auto* text = lua_tolstring( state, -1, &length );

					if ( text != nullptr ) {
						out.emplace_back( text, length );
					}
				}

				lua_pop( state, 1 );
			}
		}

		lua_pop( state, 1 );

		return out;
	}

}
