#include "mcode/ext/lua_host.hxx"
#include "mcode/ext/lua_host_internal.hxx"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

namespace mcode {

	auto lua_host::run( const std::string_view chunk,
		const std::string_view chunk_name ) -> status {
		if ( auto ready = seal( ); !ready ) {
			return ready;
		}

		auto message = std::string{ };

		if ( !ext::detail::load_chunk( thread_, chunk, chunk_name, message ) ) {
			return std::unexpected( fail( errc::lua_error, "compile error: " + message ) );
		}

		auto budget = budget_scope{ this };

		// a chunk's return values are discarded: leaving them would grow the stack per call.
		if ( lua_pcall( thread_, 0, 0, 0 ) != 0 ) {
			return std::unexpected( fail( errc::lua_error,
				"runtime error: " + ext::detail::pop_error( thread_ ) ) );
		}

		lua_settop( thread_, 0 );

		return { };
	}

	auto lua_host::run_file( const std::filesystem::path& path ) -> status {
		auto stream = std::ifstream{ path, std::ios::binary };

		if ( !stream ) {
			return std::unexpected( fail( errc::io, "cannot open " + path.string( ) ) );
		}

		auto buffer = std::ostringstream{ };
		buffer << stream.rdbuf( );

		const auto source = buffer.str( );

		return run( source, path.string( ) );
	}

	auto lua_host::eval_to_string( const std::string_view expression ) -> result< std::string > {
		if ( auto ready = seal( ); !ready ) {
			return std::unexpected( ready.error( ) );
		}

		auto chunk = std::string{ "return " };
		chunk.append( expression );

		auto message = std::string{ };

		if ( !ext::detail::load_chunk( thread_, chunk, "=(eval)", message ) ) {
			return std::unexpected( fail( errc::lua_error, "compile error: " + message ) );
		}

		auto budget = budget_scope{ this };

		if ( lua_pcall( thread_, 0, 1, 0 ) != 0 ) {
			return std::unexpected( fail( errc::lua_error,
				"runtime error: " + ext::detail::pop_error( thread_ ) ) );
		}

		auto length = std::size_t{ 0 };
		auto* text = lua_tolstring( thread_, -1, &length );

		if ( text != nullptr ) {
			auto out = std::string{ text, length };

			lua_pop( thread_, 1 );

			return out;
		}

		// lua_tolstring handles only strings and numbers, so booleans and nil go via tostring.
		lua_getglobal( thread_, "tostring" );
		lua_pushvalue( thread_, -2 );

		if ( lua_pcall( thread_, 1, 1, 0 ) != 0 ) {
			const auto failure = ext::detail::pop_error( thread_ );

			lua_pop( thread_, 1 );

			return std::unexpected( fail( errc::lua_error, "tostring failed: " + failure ) );
		}

		text = lua_tolstring( thread_, -1, &length );

		auto out = ( text != nullptr ) ? std::string{ text, length } : std::string{ };

		lua_pop( thread_, 2 );

		return out;
	}

	auto lua_host::call_global( const std::string_view name, const std::string_view argument )
		-> result< std::string > {
		if ( auto ready = seal( ); !ready ) {
			return std::unexpected( ready.error( ) );
		}

		const auto key = std::string{ name };

		lua_getglobal( thread_, key.c_str( ) );

		if ( lua_isfunction( thread_, -1 ) == 0 ) {
			lua_pop( thread_, 1 );

			return std::unexpected( fail( errc::lua_error, "no function named '" + key + "'" ) );
		}

		lua_pushlstring( thread_, argument.data( ), argument.size( ) );

		auto budget = budget_scope{ this };

		if ( lua_pcall( thread_, 1, 1, 0 ) != 0 ) {
			return std::unexpected( fail( errc::lua_error,
				"runtime error: " + ext::detail::pop_error( thread_ ) ) );
		}

		auto length = std::size_t{ 0 };
		auto* text = lua_tolstring( thread_, -1, &length );

		if ( text != nullptr ) {
			auto out = std::string{ text, length };

			lua_pop( thread_, 1 );

			return out;
		}

		lua_getglobal( thread_, "tostring" );
		lua_pushvalue( thread_, -2 );

		if ( lua_pcall( thread_, 1, 1, 0 ) != 0 ) {
			const auto failure = ext::detail::pop_error( thread_ );

			lua_pop( thread_, 1 );

			return std::unexpected( fail( errc::lua_error, "tostring failed: " + failure ) );
		}

		text = lua_tolstring( thread_, -1, &length );

		auto out = ( text != nullptr ) ? std::string{ text, length } : std::string{ };

		lua_pop( thread_, 2 );

		return out;
	}

	auto ext::detail::module_require( lua_State* state ) -> int {
		const auto* path = luaL_checkstring( state, 1 );

		lua_getfield( state, LUA_REGISTRYINDEX, "mcode.modules" );

		if ( lua_istable( state, -1 ) ) {
			lua_getfield( state, -1, path );

			if ( lua_isnil( state, -1 ) == 0 ) {
				lua_remove( state, -2 );

				return 1;
			}

			lua_pop( state, 1 );
		} else {
			// the cache store below indexes -2, so the slot must be a table, not a leftover.
			lua_pop( state, 1 );

			lua_newtable( state );
			lua_pushvalue( state, -1 );
			lua_setfield( state, LUA_REGISTRYINDEX, "mcode.modules" );
		}

		auto* loader = ext::detail::loader_from( state );

		if ( loader == nullptr || !*loader ) {
			// the modules table goes, not the path: `path` must stay reachable for the message.
			lua_remove( state, -2 );

			lua_pushfstring( state, "module not found: %s", path );

			lua_error( state );
		}

		// the loader owns resolution and confinement; a refused path is a load error.
		auto source = ( *loader )( path );

		if ( !source ) {
			lua_remove( state, -2 );

			lua_pushfstring( state, "module not found: %s", path );

			lua_error( state );
		}

		auto message = std::string{ };

		if ( !ext::detail::load_chunk( state, *source, path, message ) ) {
			lua_remove( state, -2 );

			lua_pushlstring( state, message.data( ), message.size( ) );

			lua_error( state );
		}

		if ( lua_pcall( state, 0, 1, 0 ) != 0 ) {
			lua_remove( state, -2 );

			lua_error( state );
		}

		// caches the module's return value so its body runs once per VM.
		const auto cache_index = lua_absindex( state, -2 );

		lua_pushvalue( state, -1 );
		lua_setfield( state, cache_index, path );
		lua_remove( state, -2 );

		return 1;
	}

	auto lua_host::version_string( ) const -> std::string {
		if ( state_ == nullptr ) {
			return "unavailable";
		}

		lua_getglobal( state_, "_VERSION" );

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state_, -1, &length );
		auto out = ( text != nullptr ) ? std::string{ text, length } : std::string{ "Luau" };

		lua_pop( state_, 1 );

		return out;
	}

}
