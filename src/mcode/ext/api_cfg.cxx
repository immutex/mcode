#include "mcode/ext/api_cfg.hxx"

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		// The config prefix an extension's settings live under. One section per
		// extension, so `cfg.get("key")` reads `extensions.<name>.key` and no
		// extension can read another's section or the harness's own keys.
		auto extension_key( const std::string_view name, const std::string_view key )
			-> std::string {
			auto out = std::string{ "extensions." };
			out += name;
			out += '.';
			out += key;

			return out;
		}

		// Pushes the TOML value, or the caller's default when the key is absent.
		// A value the merged config holds is copied out as plain data; the
		// extension never sees a reference into the config.
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
			// No config was installed with the surface. Absent is the honest
			// answer, and the caller's default is exactly the contract for it.
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
