#include "mcode/ext/lua_host.hxx"
#include "mcode/ext/lua_host_internal.hxx"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

namespace mcode {

	auto lua_host::run( const std::string_view chunk, const std::string_view chunk_name ) -> status {
		if ( auto ready = seal( ); !ready ) {
			return ready;
		}

		auto message = std::string{ };

		if ( !ext::detail::load_chunk( thread_, chunk, chunk_name, message ) ) {
			return std::unexpected( fail( errc::lua_error, "compile error: " + message ) );
		}

		auto budget = budget_scope{ this };

		if ( lua_pcall( thread_, 0, LUA_MULTRET, 0 ) != 0 ) {
			return std::unexpected( fail( errc::lua_error,
				"runtime error: " + ext::detail::pop_error( thread_ ) ) );
		}

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
			return std::unexpected( fail( errc::lua_error, "runtime error: " + ext::detail::pop_error( thread_ ) ) );
		}

		auto length = std::size_t{ 0 };
		auto* text = lua_tolstring( thread_, -1, &length );

		if ( text != nullptr ) {
			auto out = std::string{ text, length };

			lua_pop( thread_, 1 );

			return out;
		}

		// lua_tolstring converts only strings and numbers. Booleans and nil --
		// what most probes return -- must go through the VM's own tostring.
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
			return std::unexpected( fail( errc::lua_error, "runtime error: " + ext::detail::pop_error( thread_ ) ) );
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
		}

		// A miss. The cache table is on top of the stack and stays there.
		auto* loader = ext::detail::loader_from( state );

		if ( loader == nullptr || !*loader ) {
			lua_settop( state, 0 );

			lua_pushfstring( state, "module not found: %s", path );

			lua_error( state );
		}

		// The loader owns path resolution and confinement. A path it refuses is
		// a load error, never a filesystem fallback.
		auto source = ( *loader )( path );

		if ( !source ) {
			lua_settop( state, 0 );

			lua_pushfstring( state, "module not found: %s", path );

			lua_error( state );
		}

		auto message = std::string{ };

		if ( !ext::detail::load_chunk( state, *source, path, message ) ) {
			// The error is already on the stack, above the cache.
			lua_remove( state, -2 );

			lua_error( state );
		}

		// Stack is [cache, chunk]. The chunk is on top, so this calls the chunk;
		// the cache stays below as the frame's only other slot.
		if ( lua_pcall( state, 0, 1, 0 ) != 0 ) {
			lua_remove( state, -2 );

			lua_error( state );
		}

		// Stack is [cache, result]. Cache the module's return value and leave it
		// as the result, so a module body runs once per VM however many times it
		// is required.
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
