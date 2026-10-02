#include "mcode/ext/api_cfg.hxx"

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		// scoped to `extensions.<name>.`, so one extension cannot read another's section.
		auto extension_key( const std::string_view name, const std::string_view key )
			-> std::string {
			auto out = std::string{ "extensions." };
			out += name;
			out += '.';
			out += key;

			return out;
		}

		auto push_value_or_default( lua_State* state,
			const mcode::toml::value& value ) -> void {
			switch ( value.kind ) {
				case mcode::toml::value_kind::string: {
					lua_pushlstring( state, value.text.data( ), value.text.size( ) );

					break;
				}
				case mcode::toml::value_kind::integer: {
					lua_pushnumber( state, static_cast< double >( value.integer ) );

					break;
				}
				case mcode::toml::value_kind::floating: {
					lua_pushnumber( state, value.floating );

					break;
				}
				case mcode::toml::value_kind::boolean: {
					lua_pushboolean( state, value.boolean ? 1 : 0 );

					break;
				}
				case mcode::toml::value_kind::array: {
					lua_createtable( state, 0, static_cast< int >( value.items.size( ) ) );

					for ( auto index = std::size_t{ 0 }; index < value.items.size( ); ++index ) {
						push_value_or_default( state, value.items[ index ] );
						lua_rawseti( state, -2, static_cast< int >( index + 1 ) );
					}

					break;
				}
			}
		}

	}

	auto handle_cfg_get( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.cfg.get expects a key string" );
			lua_error( state );
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );
		const auto key = std::string{ text != nullptr ? text : "", length };

		if ( key.empty( ) ) {
			lua_pushliteral( state, "mcode.cfg.get: the key must not be empty" );
			lua_error( state );
		}

		if ( self->config( ) == nullptr ) {
			lua_pushvalue( state, 2 );

			return 1;
		}

		const auto path = extension_key( self->manifest( ).name, key );

		const auto& values = self->config( )->keys( );
		const auto found = values.find( path );

		if ( found != values.end( ) ) {
			push_value_or_default( state, found->second );

			return 1;
		}

		lua_pushvalue( state, 2 );

		return 1;
	}

}
