#include "mcode/ext/lua_host.hxx"

#include <lua.hpp>

#include <map>
#include <string>
#include <utility>

namespace mcode {

	namespace {

		char g_hook_registry_key = 0;

		auto pop_error( lua_State* state ) -> std::string {
			auto length = std::size_t{ 0 };
			const auto* message = lua_tolstring( state, -1, &length );
			auto out = ( message != nullptr ) ? std::string{ message, length } : std::string{ "unknown Lua error" };

			lua_pop( state, 1 );

			return out;
		}

		auto host_function_dispatch( lua_State* state ) -> int {
			auto* function = static_cast< host_function* >( lua_touserdata( state, lua_upvalueindex( 1 ) ) );

			if ( function == nullptr || !*function ) {
				lua_pushliteral( state, "host function is not bound" );

				return lua_error( state );
			}

			auto args = std::string{ };

			if ( lua_gettop( state ) >= 1 && lua_isstring( state, 1 ) != 0 ) {
				auto length = std::size_t{ 0 };
				const auto* text = lua_tolstring( state, 1, &length );
				args.assign( text, length );
			}

			auto outcome = ( *function )( args );

			if ( !outcome ) {
				const auto message = std::string{ "host function failed: " } +
					std::string{ to_string( outcome.error( ).code ) } + ": " + outcome.error( ).msg;

				lua_pushlstring( state, message.data( ), message.size( ) );

				return lua_error( state );
			}

			lua_pushlstring( state, outcome->data( ), outcome->size( ) );

			return 1;
		}

		auto instruction_hook( lua_State* state, lua_Debug* debug ) -> void {
			( void )debug;

			lua_pushlightuserdata( state, &g_hook_registry_key );
			lua_rawget( state, LUA_REGISTRYINDEX );

			auto* hook = static_cast< detail::hook_state* >( lua_touserdata( state, -1 ) );
			lua_pop( state, 1 );

			if ( hook == nullptr ) {
				return;
			}

			++hook->counter;

			if ( hook->budget != 0 && hook->counter > hook->budget ) {
				luaL_error( state, "instruction budget exhausted (%d instructions)",
					static_cast< int >( hook->budget ) );
			}
		}

	}

	lua_host::~lua_host( ) {
		if ( state_ != nullptr ) {
			lua_close( state_ );
			state_ = nullptr;
		}
	}

	lua_host::lua_host( lua_host&& other ) noexcept
		: state_( other.state_ )
		, jit_enabled_( other.jit_enabled_ )
		, extension_name_( std::move( other.extension_name_ ) )
		, hook_( std::move( other.hook_ ) )
		, host_functions_( std::move( other.host_functions_ ) ) {
		other.state_ = nullptr;
	}

	auto lua_host::operator=( lua_host&& other ) noexcept -> lua_host& {
		if ( this != &other ) {
			if ( state_ != nullptr ) {
				lua_close( state_ );
			}

			state_ = other.state_;
			jit_enabled_ = other.jit_enabled_;
			extension_name_ = std::move( other.extension_name_ );
			hook_ = std::move( other.hook_ );
			host_functions_ = std::move( other.host_functions_ );
			other.state_ = nullptr;
		}

		return *this;
	}

	auto lua_host::instructions_executed( ) const noexcept -> std::uint64_t {
		return hook_ != nullptr ? hook_->counter : 0;
	}

	auto lua_host::create( lua_host_options options ) -> result< lua_host > {
		auto host = lua_host{ };
		host.extension_name_ = std::move( options.extension_name );
		host.host_functions_ = std::make_unique< std::map< std::string, host_function, std::less<> > >( );

		auto* state = luaL_newstate( );

		if ( state == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "luaL_newstate failed (out of memory)" ) );
		}

		host.state_ = state;
		luaL_openlibs( state );

		if ( !options.enable_jit ) {
			if ( luaL_dostring( state, "if jit then jit.off() end" ) != 0 ) {
				const auto message = pop_error( state );
				lua_close( state );
				host.state_ = nullptr;

				return std::unexpected( fail( errc::lua_error, "failed to disable JIT: " + message ) );
			}
		}

		host.jit_enabled_ = false;

		if ( luaL_dostring( state, "return (jit and jit.status() and true) or false" ) == 0 ) {
			host.jit_enabled_ = lua_toboolean( state, -1 ) != 0;
			lua_pop( state, 1 );
		} else {
			lua_pop( state, 1 );
		}

		lua_newtable( state );
		lua_setglobal( state, "mcode" );

		if ( options.instruction_budget != 0 ) {
			host.hook_ = std::make_unique< detail::hook_state >( );
			host.hook_->budget = options.instruction_budget;

			lua_pushlightuserdata( state, &g_hook_registry_key );
			lua_pushlightuserdata( state, host.hook_.get( ) );
			lua_rawset( state, LUA_REGISTRYINDEX );
			lua_sethook( state, instruction_hook, LUA_MASKCOUNT, 10000 );
		}

		return host;
	}

	auto lua_host::run( const std::string_view chunk, const std::string_view chunk_name ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		const auto name = std::string{ chunk_name };

		if ( luaL_loadbuffer( state_, chunk.data( ), chunk.size( ), name.c_str( ) ) != 0 ) {
			const auto message = pop_error( state_ );

			return std::unexpected( fail( errc::lua_error, "compile error: " + message ) );
		}

		if ( lua_pcall( state_, 0, LUA_MULTRET, 0 ) != 0 ) {
			const auto message = pop_error( state_ );

			return std::unexpected( fail( errc::lua_error, "runtime error: " + message ) );
		}

		return { };
	}

	auto lua_host::run_file( const std::filesystem::path& path ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		const auto text = path.string( );

		if ( luaL_loadfile( state_, text.c_str( ) ) != 0 ) {
			const auto message = pop_error( state_ );

			return std::unexpected( fail( errc::io, "cannot load " + text + ": " + message ) );
		}

		if ( lua_pcall( state_, 0, LUA_MULTRET, 0 ) != 0 ) {
			const auto message = pop_error( state_ );

			return std::unexpected( fail( errc::lua_error, text + ": " + message ) );
		}

		return { };
	}

	auto lua_host::eval_to_string( const std::string_view expression ) -> result< std::string > {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		auto chunk = std::string{ "return " };
		chunk.append( expression );

		if ( luaL_loadbuffer( state_, chunk.data( ), chunk.size( ), "=(eval)" ) != 0 ) {
			return std::unexpected( fail( errc::lua_error, "compile error: " + pop_error( state_ ) ) );
		}

		if ( lua_pcall( state_, 0, 1, 0 ) != 0 ) {
			return std::unexpected( fail( errc::lua_error, "runtime error: " + pop_error( state_ ) ) );
		}

		auto length = std::size_t{ 0 };
		const auto* text = lua_tolstring( state_, -1, &length );
		auto out = ( text != nullptr ) ? std::string{ text, length } : std::string{ "<non-string result>" };

		lua_pop( state_, 1 );

		return out;
	}

	auto lua_host::register_host_function( const std::string_view name, host_function function ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		if ( name.empty( ) ) {
			return std::unexpected( fail( errc::config, "host function name must not be empty" ) );
		}

		const auto key = std::string{ name };
		const auto [ entry, inserted ] = host_functions_->try_emplace( key, std::move( function ) );

		if ( !inserted ) {
			return std::unexpected( fail( errc::config, "duplicate host function '" + key + "'" ) );
		}

		lua_getglobal( state_, "mcode" );

		if ( lua_isnil( state_, -1 ) != 0 ) {
			lua_pop( state_, 1 );
			lua_newtable( state_ );
			lua_pushvalue( state_, -1 );
			lua_setglobal( state_, "mcode" );
		}

		lua_pushlightuserdata( state_, &entry->second );
		lua_pushcclosure( state_, host_function_dispatch, 1 );
		lua_setfield( state_, -2, key.c_str( ) );
		lua_pop( state_, 1 );

		return { };
	}

	auto lua_host::set_global_string( const std::string_view name, const std::string_view text ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		const auto key = std::string{ name };

		lua_pushlstring( state_, text.data( ), text.size( ) );
		lua_setglobal( state_, key.c_str( ) );

		return { };
	}

	auto lua_host::version_string( ) const -> std::string {
		if ( state_ == nullptr ) {
			return "unavailable";
		}

		auto version = std::string{ LUAJIT_VERSION };

		if ( luaL_dostring( state_, "return (jit and jit.version) or _VERSION" ) == 0 ) {
			auto length = std::size_t{ 0 };
			const auto* text = lua_tolstring( state_, -1, &length );

			if ( text != nullptr ) {
				version.assign( text, length );
			}

			lua_pop( state_, 1 );
		} else {
			lua_pop( state_, 1 );
		}

		return version;
	}

}
