#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

#include <cmath>
#include <string>
#include <string_view>

#include "lua.h"
#include "lualib.h"

#include "mcode/ext/lua_json.hxx"

namespace mcode::ext {


	auto api_surface::handle_on( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.on: the event name must be a string" );
			lua_error( state );
		}

		auto name_length = std::size_t{ 0 };
		const auto* name_text = lua_tolstring( state, 1, &name_length );

		if ( name_text == nullptr || name_length == 0 ) {
			lua_pushliteral( state, "mcode.on: the event name must not be empty" );
			lua_error( state );
		}

		if ( lua_type( state, 2 ) != LUA_TFUNCTION ) {
			lua_pushliteral( state, "mcode.on: the handler must be a function" );
			lua_error( state );
		}

		const auto name = std::string_view{ name_text, name_length };

		if ( ext::has_reserved_prefix( name ) ) {
			const auto message = std::string{ "mcode.on: '" } + std::string{ name } +
				"' is not a session event, but '" +
				std::string{ name.substr( 0, name.find( '.' ) ) } +
				"' is a session event namespace -- did you mistype one?";

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		if ( !ext::session_event_kind( name ) && name.find( '.' ) == std::string_view::npos ) {
			const auto message = std::string{ "mcode.on: '" } + std::string{ name } +
				"' is not a session event and is not namespaced like a custom event "
				"(expected something like 'myext.ready')";

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		lua_pushvalue( state, 2 );
		const auto reference = lua_ref( state, -1 );
		lua_pop( state, 1 );

		auto identifier = self->hooks_->subscribe( *self->host_, name, reference,
			self->manifest_.name );

		if ( !identifier ) {
			lua_unref( state, reference );

			lua_pushlstring( state, identifier.error( ).msg.data( ),
				identifier.error( ).msg.size( ) );
			lua_error( state );
		}

		lua_pushnumber( state, static_cast< double >( *identifier ) );

		return 1;
	}

	auto api_surface::handle_off( lua_State* state ) -> int {
		auto* self = surface_from( state );

		const auto number = luaL_checknumber( state, 1 );

		if ( number < 1.0 || number != std::floor( number ) ) {
			return 0;
		}

		const auto identifier = static_cast< std::uint64_t >( number );

		self->hooks_->unsubscribe( identifier );

		return 0;
	}

	auto api_surface::handle_emit( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.emit: the event name must be a string" );
			lua_error( state );
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );

		const auto name = std::string_view{ text != nullptr ? text : "", length };

		{
			const auto separator = name.find( '.' );
			const auto prefix = separator == std::string_view::npos
				? name : name.substr( 0, separator );

			if ( prefix != self->manifest_.name ) {
				const auto message = std::string{ "mcode.emit: '" } + std::string{ name } +
					"' is outside this extension's namespace ('" + self->manifest_.name +
					".')";

				lua_pushlstring( state, message.data( ), message.size( ) );
				lua_error( state );
			}
		}

		auto payload = std::string{ "{}" };

		if ( lua_type( state, 2 ) == LUA_TTABLE ) {
			auto rendered = json_from_lua( state, 2 );

			if ( !rendered ) {
				const auto message = std::string{ "mcode.emit: the payload is not plain data: " } +
					rendered.error( ).msg;

				lua_pushlstring( state, message.data( ), message.size( ) );
				lua_error( state );
			}

			payload = *rendered;
		}

		if ( auto emitted = self->hooks_->emit( name, payload ); !emitted ) {
			lua_pushlstring( state, emitted.error( ).msg.data( ), emitted.error( ).msg.size( ) );
			lua_error( state );
		}

		return 0;
	}

}
