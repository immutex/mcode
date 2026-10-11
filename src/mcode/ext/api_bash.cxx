#include "mcode/ext/api_bash.hxx"

#include <cstdint>
#include <string>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_gate.hxx"
#include "mcode/ext/api_internal.hxx"
#include "mcode/ext/lua_json.hxx"
#include "mcode/support/json.hxx"

namespace mcode::ext {

	namespace {

		// A command longer than this is a script, and a script belongs in a file the
		// extension ships rather than in a string it builds.
		constexpr std::size_t MAX_COMMAND_CHARS = 32 * 1024;

		constexpr std::int64_t MIN_TIMEOUT_MS = 1000;
		constexpr std::int64_t MAX_TIMEOUT_MS = 600'000;
		constexpr std::int64_t DEFAULT_TIMEOUT_MS = 120'000;

	}

	// `mcode.bash(command, opts)` -> { ok, exit_code, output, ... }, err
	//
	// The one way an extension runs a process: it routes to the core `bash` handler,
	// so the sandbox, the gate and the output cap are shared. Needs `spawn`.
	auto handle_bash( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( !manifest_allows( *self, "spawn" ) ) {
			return deny_permission( state, "spawn", "mcode.bash" );
		}

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushnil( state );
			lua_pushliteral( state, "mcode.bash expects the command as a string" );

			return 2;
		}

		if ( !argument_absent( state, 2 ) && lua_type( state, 2 ) != LUA_TTABLE ) {
			lua_pushnil( state );
			lua_pushliteral( state, "mcode.bash: opts must be a table or nil" );

			return 2;
		}

		auto* runner = self->bash_runner( );

		if ( runner == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state,
				"no command runner was installed with this surface; the harness "
				"cannot run a process here" );

			return 2;
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );

		if ( text == nullptr || length == 0 ) {
			lua_pushnil( state );
			lua_pushliteral( state, "command is required" );

			return 2;
		}

		if ( length > MAX_COMMAND_CHARS ) {
			lua_pushnil( state );
			lua_pushliteral( state, "command is too long" );

			return 2;
		}

		auto command = std::string{ text, length };
		auto timeout_ms = DEFAULT_TIMEOUT_MS;

		if ( lua_type( state, 2 ) == LUA_TTABLE ) {
			const auto options = lua_absindex( state, 2 );

			lua_getfield( state, options, "timeout_ms" );

			if ( lua_type( state, -1 ) == LUA_TNUMBER ) {
				const auto requested = static_cast< std::int64_t >( lua_tonumber( state, -1 ) );

				if ( requested >= MIN_TIMEOUT_MS ) {
					timeout_ms = requested < MAX_TIMEOUT_MS ? requested : MAX_TIMEOUT_MS;
				}
			}

			lua_pop( state, 1 );
		}

		const auto ran = ( *runner )( command, timeout_ms );

		if ( !ran ) {
			lua_pushnil( state );
			lua_pushlstring( state, ran.error( ).msg.data( ), ran.error( ).msg.size( ) );

			return 2;
		}

		// The core handler answers with its own JSON, so it is decoded here rather
		// than handed through: an extension should not need a JSON parser to read a
		// command's exit code. The schema is the core tool's, unchanged.
		if ( auto pushed = push_json( state, *ran ); !pushed ) {
			lua_pushnil( state );
			lua_pushliteral( state, "the command result could not be decoded" );

			return 2;
		}

		return 1;
	}

}
