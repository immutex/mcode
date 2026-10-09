#include "mcode/ext/api_fs.hxx"

#include "lua.h"

#include "mcode/ext/api_gate.hxx"
#include "mcode/ext/api_internal.hxx"
#include "mcode/support/json.hxx"
#include "mcode/tools/file_tools.hxx"
#include "mcode/tools/tool_args.hxx"

namespace mcode::ext {

	auto handle_fs_read( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.fs.read expects a path string" );
			lua_error( state );
		}

		if ( !manifest_allows( *self, "fs_read" ) ) {
			return deny_permission( state, "fs_read", "mcode.fs.read" );
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, 1, &length );
		const auto path = std::string{ text != nullptr ? text : "", length };

		// Built with the JSON writer, not by hand. `json::append_escaped` writes the
		// escaped BODY of a string and never its surrounding quotes, so the previous
		// `"{\"path\":" + escaped + "}"` produced `{"path":/tmp/x}` -- not valid
		// JSON. `tool_args::parse` is strict, so every call took the failure branch
		// and `mcode.fs.read` could never succeed at all.
		auto arguments_document = mcode::json::document::make_object( );
		arguments_document.set_string( "path", path );

		const auto arguments = arguments_document.dump( );

		if ( !arguments ) {
			lua_pushnil( state );
			lua_pushliteral( state, "mcode.fs.read could not build its arguments" );

			return 2;
		}

		auto parsed = mcode::tools::tool_args::parse( *arguments );

		if ( !parsed ) {
			lua_pushnil( state );
			lua_pushlstring( state, parsed.error( ).msg.data( ), parsed.error( ).msg.size( ) );

			return 2;
		}

		auto outcome = self->file_context( )
			? mcode::tools::handle_read( *parsed, *self->file_context( ) )
			: std::unexpected( mcode::fail( mcode::errc::config,
				"no file context was installed with this surface" ) );

		if ( !outcome ) {
			lua_pushnil( state );
			lua_pushlstring( state, outcome.error( ).msg.data( ), outcome.error( ).msg.size( ) );

			return 2;
		}

		lua_pushlstring( state, outcome->data( ), outcome->size( ) );

		return 1;
	}

	auto handle_fs_write( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.fs.write expects a path string" );
			lua_error( state );
		}

		if ( lua_type( state, 2 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.fs.write expects content as a string" );
			lua_error( state );
		}

		if ( !manifest_allows( *self, "fs_write" ) ) {
			return deny_permission( state, "fs_write", "mcode.fs.write" );
		}

		auto length = std::size_t{ 0 };
		const auto* path_text = lua_tolstring( state, 1, &length );
		const auto path = std::string{ path_text != nullptr ? path_text : "", length };

		length = 0;
		const auto* data_text = lua_tolstring( state, 2, &length );
		const auto data = std::string{ data_text != nullptr ? data_text : "", length };

		auto arguments_document = mcode::json::document::make_object( );
		arguments_document.set_string( "path", path );
		arguments_document.set_string( "content", data );

		const auto arguments = arguments_document.dump( );

		if ( !arguments ) {
			lua_pushnil( state );
			lua_pushliteral( state, "mcode.fs.write could not build its arguments" );

			return 2;
		}

		auto parsed = mcode::tools::tool_args::parse( *arguments );

		if ( !parsed ) {
			lua_pushnil( state );
			lua_pushlstring( state, parsed.error( ).msg.data( ), parsed.error( ).msg.size( ) );

			return 2;
		}

		auto outcome = self->file_context( )
			? mcode::tools::handle_write( *parsed, *self->file_context( ) )
			: std::unexpected( mcode::fail( mcode::errc::config,
				"no file context was installed with this surface" ) );

		if ( !outcome ) {
			lua_pushnil( state );
			lua_pushlstring( state, outcome.error( ).msg.data( ), outcome.error( ).msg.size( ) );

			return 2;
		}

		lua_pushboolean( state, 1 );

		return 1;
	}

}
