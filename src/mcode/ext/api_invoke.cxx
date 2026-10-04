#include "mcode/ext/api.hxx"

#include <string>
#include <utility>

#include "lua.h"
#include "lualib.h"

#include "mcode/ext/lua_json.hxx"

namespace mcode::ext {

	auto api_surface::release_all( lua_host& host ) -> void {
		auto* state = host.raw( );

		for ( const auto& tool : tools_ ) {
			if ( registry_ != nullptr ) {
				registry_->remove( tool.name );
			}

			if ( state != nullptr ) {
				lua_unref( state, tool.function_reference );
			}
		}

		tools_.clear( );
		by_name_.clear( );
	}

	auto api_surface::invoke( const std::string_view tool_name,
		const std::string_view arguments_json ) -> result< std::string > {
		if ( host_ == nullptr ) {
			return std::unexpected( fail( errc::config, "the API surface is not installed" ) );
		}

		const auto found = by_name_.find( tool_name );
		const auto* tool = found != by_name_.end( ) ? &tools_[ found->second ] : nullptr;

		if ( tool == nullptr ) {
			return std::unexpected( fail( errc::config,
				"no tool named '" + std::string{ tool_name } + "'" ) );
		}

		auto* state = host_->raw( );

		if ( state == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "the host has no thread" ) );
		}

		// budgets the call: a model-invoked extension tool must not hang the session.
		auto budget = lua_host::budget_scope{ host_ };

		const auto depth = lua_gettop( state );

		lua_getref( state, tool->function_reference );

		if ( lua_type( state, -1 ) != LUA_TFUNCTION ) {
			lua_settop( state, depth );

			return std::unexpected( fail( errc::lua_error,
				"the tool's function is no longer live" ) );
		}

		if ( auto pushed = push_json( state, arguments_json ); !pushed ) {
			lua_settop( state, depth );

			return std::unexpected( fail( errc::config,
				"arguments are not a JSON object: " + pushed.error( ).msg ) );
		}

		lua_createtable( state, 0, 3 );
		lua_pushlstring( state, tool->owner.data( ), tool->owner.size( ) );
		lua_setfield( state, -2, "extension" );
		lua_pushnumber( state, 0 );
		lua_setfield( state, -2, "seq" );

		if ( lua_pcall( state, 2, 2, 0 ) != 0 ) {
			auto length = std::size_t{ 0 };
			const auto* message = lua_tolstring( state, -1, &length );
			auto text = ( message != nullptr ) ? std::string{ message, length }
				: std::string{ "unknown Luau error" };

			lua_settop( state, depth );

			return std::unexpected( fail( errc::lua_error, text ) );
		}

		if ( lua_type( state, -1 ) != LUA_TNIL ) {
			auto length = std::size_t{ 0 };
			const auto* message = lua_tolstring( state, -1, &length );
			auto text = ( message != nullptr ) ? std::string{ message, length }
				: std::string{ "the tool failed" };

			lua_settop( state, depth );

			return std::unexpected( fail( errc::lua_error, text ) );
		}

		if ( lua_type( state, -2 ) == LUA_TNIL ) {
			lua_settop( state, depth );

			return std::unexpected( fail( errc::lua_error,
				"the tool returned no result and no error" ) );
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state, -2, &length );

		if ( text == nullptr ) {
			const auto* type_name = lua_typename( state, lua_type( state, -2 ) );
			const auto described = std::string{ type_name != nullptr ? type_name : "unknown" };

			lua_settop( state, depth );

			return std::unexpected( fail( errc::lua_error,
				"the tool must return a string, but returned a " + described ) );
		}

		auto out = std::string{ text, length };

		lua_settop( state, depth );

		return out;
	}

}
