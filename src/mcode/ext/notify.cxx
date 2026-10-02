#include "mcode/ext/notify.hxx"

#include <cstdio>
#include <string>
#include <utility>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		inline constexpr auto MAX_NOTIFY_CHARS = std::size_t{ 2'048 };

		const char* const NOTIFY_LEVELS[] = { "info", "warn", "error" };

		[[nodiscard]] auto is_valid_level( const std::string_view level ) -> bool {
			for ( const auto* candidate : NOTIFY_LEVELS ) {
				if ( level == candidate ) {
					return true;
				}
			}

			return false;
		}

	}

	auto handle_notify( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.notify expects the message as a string" );
			lua_error( state );
		}

		auto level = std::string{ "info" };

		if ( lua_type( state, 2 ) != LUA_TNIL ) {
			if ( lua_type( state, 2 ) != LUA_TSTRING ) {
				lua_pushliteral( state, "mcode.notify: the level must be a string or "
					"nil" );
				lua_error( state );
			}

			auto length = std::size_t{ 0 };
			const auto* level_text = lua_tolstring( state, 2, &length );

			level.assign( level_text != nullptr ? level_text : "", length );

			if ( !is_valid_level( level ) ) {
				lua_pushliteral( state, "mcode.notify: the level must be one of "
					"'info', 'warn', 'error'" );
				lua_error( state );
			}
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );
		auto message = std::string{ text != nullptr ? text : "", length };

		if ( message.size( ) > MAX_NOTIFY_CHARS ) {
			message.resize( MAX_NOTIFY_CHARS );
		}

		if ( auto* notifier = self->notifier( ) ) {
			( *notifier )( self->manifest( ).name, level, message );

			return 0;
		}

		std::fprintf( stderr, "[%s] notify (%s): %s\n",
			self->manifest( ).name.c_str( ), level.c_str( ), message.c_str( ) );

		return 0;
	}

}
