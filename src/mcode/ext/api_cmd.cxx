#include "mcode/ext/api_cmd.hxx"

#include <algorithm>
#include <string>
#include <utility>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		// The longest name a command may carry. A slash line is typed by a human
		// at a prompt, so anything past this is a typo rather than a name, and
		// an unbounded name would let one extension fill the completion list.
		inline constexpr auto MAX_COMMAND_NAME_LENGTH = std::size_t{ 64 };

		// The cap on completions one invocation may return. The completion list
		// is rendered into the prompt line, so an unbounded return would let an
		// extension push arbitrary text at the user.
		inline constexpr auto MAX_COMPLETIONS = 32;

		auto is_valid_command_name( const std::string_view name ) -> bool {
			if ( name.empty( ) || name.size( ) > MAX_COMMAND_NAME_LENGTH ) {
				return false;
			}

			for ( const auto character : name ) {
				const auto allowed = ( character >= 'a' && character <= 'z' )
					|| ( character >= 'A' && character <= 'Z' )
					|| ( character >= '0' && character <= '9' )
					|| character == '-' || character == '_';

				if ( !allowed ) {
					return false;
				}
			}

			return true;
		}

	}

	auto command_registry::add( registered_command command )
		-> mcode::result< int > {
		if ( !is_valid_command_name( command.name ) ) {
			return std::unexpected( mcode::fail( mcode::errc::config,
				"mcode.cmd.register: '" + command.name + "' is not a valid command name "
				"(letters, digits, '-' and '_', at most "
				+ std::to_string( MAX_COMMAND_NAME_LENGTH ) + " characters)" ) );
		}

		if ( find( command.name ) != nullptr ) {
			return std::unexpected( mcode::fail( mcode::errc::config,
				"mcode.cmd.register: a command named '" + command.name
				+ "' is already registered" ) );
		}

		commands_.push_back( std::move( command ) );

		return static_cast< int >( commands_.size( ) );
	}

	auto command_registry::remove_owner( const std::string_view owner ) -> std::size_t {
		auto removed = std::size_t{ 0 };

		for ( auto iterator = commands_.begin( ); iterator != commands_.end( ); ) {
			if ( iterator->owner != owner ) {
				++iterator;

				continue;
			}

			iterator = commands_.erase( iterator );
			++removed;
		}

		return removed;
	}

	auto command_registry::find( const std::string_view name ) const
		-> const registered_command* {
		const auto found = std::find_if( commands_.begin( ), commands_.end( ),
			[ & ]( const registered_command& command ) {
				return command.name == name;
			} );

		return found != commands_.end( ) ? &*found : nullptr;
	}

	auto invoke_command( api_surface& surface, const registered_command& command,
		const std::string_view arguments ) -> mcode::result< std::string > {
		auto* host = surface.raw_host( );

		if ( host == nullptr ) {
			return std::unexpected( mcode::fail( mcode::errc::config,
				"the API surface is not installed" ) );
		}

		auto* state = host->raw( );

		if ( state == nullptr ) {
			return std::unexpected( mcode::fail( mcode::errc::lua_error,
				"the host has no thread" ) );
		}

		auto budget = lua_host::budget_scope{ host };

		const auto depth = lua_gettop( state );

		lua_getref( state, command.function_reference );

		if ( lua_type( state, -1 ) != LUA_TFUNCTION ) {
			lua_settop( state, depth );

			return std::unexpected( mcode::fail( mcode::errc::lua_error,
				"the command's function is no longer live" ) );
		}

		lua_pushlstring( state, arguments.data( ), arguments.size( ) );

		// A command handler that throws is contained, exactly as a hook handler
		// is: the user sees the message, the session continues.
		if ( lua_pcall( state, 1, 1, 0 ) != 0 ) {
			auto message = std::string{ };

			if ( lua_type( state, -1 ) == LUA_TSTRING ) {
				auto length = std::size_t{ 0 };
				const auto* text = lua_tolstring( state, -1, &length );

				if ( text != nullptr ) {
					message.assign( text, length );
				}
			}

			lua_settop( state, depth );

			return std::unexpected( mcode::fail( mcode::errc::lua_error,
				message.empty( ) ? "the command handler failed" : message ) );
		}

		auto out = std::string{ };

		if ( lua_type( state, -1 ) == LUA_TSTRING || lua_type( state, -1 ) == LUA_TNUMBER ) {
			auto length = std::size_t{ 0 };
			const auto* text = lua_tolstring( state, -1, &length );

			if ( text != nullptr ) {
				out.assign( text, length );
			}
		}

		lua_settop( state, depth );

		return out;
	}

	auto handle_cmd_register( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.cmd.register expects the command name as a "
				"string" );
			lua_error( state );
		}

		if ( lua_type( state, 2 ) != LUA_TFUNCTION ) {
			lua_pushliteral( state, "mcode.cmd.register expects a run function" );
			lua_error( state );
		}

		if ( lua_type( state, 3 ) != LUA_TNIL && lua_type( state, 3 ) != LUA_TTABLE ) {
			lua_pushliteral( state, "mcode.cmd.register: opts must be a table or nil" );
			lua_error( state );
		}

		auto length = std::size_t{ 0 };
		const auto* name_text = lua_tolstring( state, 1, &length );
		const auto name = std::string{ name_text != nullptr ? name_text : "", length };

		if ( !is_valid_command_name( name ) ) {
			const auto message = std::string{ "mcode.cmd.register: '" } + name
				+ "' is not a valid command name";

			lua_pushlstring( state, message.data( ), message.size( ) );
			lua_error( state );
		}

		auto command = registered_command{ };
		command.name = name;
		command.owner = self->manifest( ).name;

		if ( lua_type( state, 3 ) == LUA_TTABLE ) {
			command.description = read_field_string( state, 3, "description" );

			lua_getfield( state, 3, "complete" );

			if ( lua_type( state, -1 ) == LUA_TFUNCTION ) {
				command.completable = true;
				command.complete_reference = lua_ref( state, -1 );
			}

			lua_pop( state, 1 );
		}

		lua_pushvalue( state, 2 );
		command.function_reference = lua_ref( state, -1 );
		lua_pop( state, 1 );

		auto* commands = self->commands( );

		// A rejected registration must not leak the references it took. Every
		// failure path past this point releases them before raising.
		const auto release_references = [ & ]( ) {
			lua_unref( state, command.function_reference );

			if ( command.complete_reference != 0 ) {
				lua_unref( state, command.complete_reference );
			}
		};

		if ( commands == nullptr ) {
			// No registry was installed with the surface. A denied registration is
			// a contract violation here rather than an environmental failure: the
			// host built no command surface at all, and silently dropping the
			// command would leave the author believing it exists.
			release_references( );

			lua_pushliteral( state, "mcode.cmd.register: no command registry was "
				"installed with this surface" );
			lua_error( state );
		}

		auto added = commands->add( std::move( command ) );

		if ( !added ) {
			release_references( );

			lua_pushlstring( state, added.error( ).msg.data( ),
				added.error( ).msg.size( ) );
			lua_error( state );
		}

		lua_pushnumber( state, static_cast< double >( *added ) );

		return 1;
	}

}
