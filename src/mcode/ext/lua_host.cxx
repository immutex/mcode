#include "mcode/ext/lua_host.hxx"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

namespace mcode {

	namespace {

		char g_allocator_key = 0;
		char g_watchdog_key = 0;

		constexpr auto INTERRUPT_GRANULARITY = std::uint64_t{ 10'000 };

		auto pop_error( lua_State* state ) -> std::string {
			auto length = std::size_t{ 0 };
			const auto* message = lua_tolstring( state, -1, &length );
			auto out = ( message != nullptr ) ? std::string{ message, length } : std::string{ "unknown Lua error" };

			lua_pop( state, 1 );

			return out;
		}

		auto registry_pointer( lua_State* state, void* key ) -> void* {
			lua_pushlightuserdata( state, key );
			lua_rawget( state, LUA_REGISTRYINDEX );

			auto* value = lua_touserdata( state, -1 );
			lua_pop( state, 1 );

			return value;
		}

		auto allocator_from( lua_State* state ) -> detail::allocator_state* {
			return static_cast< detail::allocator_state* >( registry_pointer( state, &g_allocator_key ) );
		}

		auto watchdog_from( lua_State* state ) -> detail::watchdog_state* {
			return static_cast< detail::watchdog_state* >( registry_pointer( state, &g_watchdog_key ) );
		}

		auto luau_allocator( void* userdata, void* pointer, std::size_t old_size, std::size_t new_size )
			-> void* {
			auto* counters = static_cast< detail::allocator_state* >( userdata );

			if ( new_size == 0 ) {
				counters->bytes -= old_size;

				std::free( pointer );

				return nullptr;
			}

			// new_size == 0 is free, so the block is not in `bytes` yet only on
			// the first allocation; for a realloc the old size is already
			// counted and is being replaced, not added.
			const auto projected = counters->bytes - old_size + new_size;

			if ( counters->limit != 0 && projected > counters->limit ) {
				++counters->refusals;

				return nullptr;
			}

			auto* resized = std::realloc( pointer, new_size );

			if ( resized != nullptr ) {
				counters->bytes = projected;

				if ( counters->bytes > counters->peak ) {
					counters->peak = counters->bytes;
				}
			}

			return resized;
		}

		auto interrupt( lua_State* state, int gc ) -> void {
			// Negative gc values are real safepoints; non-negative ones are GC
			// steps, which the VM reports on the same callback.
			if ( gc >= 0 ) {
				return;
			}

			static thread_local std::uint64_t safepoints = 0;

			if ( ++safepoints % INTERRUPT_GRANULARITY != 0 ) {
				return;
			}

			auto* watchdog = watchdog_from( state );

			if ( watchdog == nullptr || !watchdog->armed ) {
				return;
			}

			if ( std::chrono::steady_clock::now( ) < watchdog->deadline ) {
				return;
			}

			watchdog->expired = true;

			luaL_error( state, "extension exceeded its time budget" );
		}

		auto host_function_dispatch( lua_State* state ) -> int {
			auto* function = static_cast< host_function* >( lua_touserdata( state, lua_upvalueindex( 1 ) ) );

			if ( function == nullptr || !*function ) {
				lua_pushliteral( state, "host function is not bound" );

				lua_error( state );
			}

			auto args = std::string{ };

			if ( lua_gettop( state ) >= 1 && lua_isstring( state, 1 ) != 0 ) {
				auto length = std::size_t{ 0 };
				const auto* text = lua_tolstring( state, 1, &length );

				if ( text != nullptr ) {
					args.assign( text, length );
				}
			}

			auto outcome = ( *function )( args );

			if ( !outcome ) {
				const auto message = std::string{ "host function failed: " } +
					std::string{ to_string( outcome.error( ).code ) } + ": " + outcome.error( ).msg;

				lua_pushlstring( state, message.data( ), message.size( ) );

				lua_error( state );
			}

			lua_pushlstring( state, outcome->data( ), outcome->size( ) );

			return 1;
		}

	}

	lua_host::~lua_host( ) {
		if ( state_ != nullptr ) {
			lua_close( state_ );
			state_ = nullptr;
			thread_ = nullptr;
		}
	}

	lua_host::lua_host( lua_host&& other ) noexcept
		: state_( other.state_ )
		, thread_( other.thread_ )
		, thread_index_( other.thread_index_ )
		, sealed_( other.sealed_ )
		, time_limit_( other.time_limit_ )
		, extension_name_( std::move( other.extension_name_ ) )
		, allocator_( std::move( other.allocator_ ) )
		, watchdog_( std::move( other.watchdog_ ) )
		, host_functions_( std::move( other.host_functions_ ) ) {
		other.state_ = nullptr;
		other.thread_ = nullptr;
		other.thread_index_ = 0;
	}

	auto lua_host::operator=( lua_host&& other ) noexcept -> lua_host& {
		if ( this != &other ) {
			if ( state_ != nullptr ) {
				lua_close( state_ );
			}

			state_ = other.state_;
			thread_ = other.thread_;
			thread_index_ = other.thread_index_;
			sealed_ = other.sealed_;
			time_limit_ = other.time_limit_;
			extension_name_ = std::move( other.extension_name_ );
			allocator_ = std::move( other.allocator_ );
			watchdog_ = std::move( other.watchdog_ );
			host_functions_ = std::move( other.host_functions_ );

			other.state_ = nullptr;
			other.thread_ = nullptr;
			other.thread_index_ = 0;
		}

		return *this;
	}

	auto lua_host::bytes_allocated( ) const noexcept -> std::uint64_t {
		return allocator_ != nullptr ? allocator_->bytes : 0;
	}

	auto lua_host::peak_bytes_allocated( ) const noexcept -> std::uint64_t {
		return allocator_ != nullptr ? allocator_->peak : 0;
	}

	auto lua_host::memory_refusals( ) const noexcept -> std::uint64_t {
		return allocator_ != nullptr ? allocator_->refusals : 0;
	}

	auto lua_host::time_expired( ) const noexcept -> bool {
		return watchdog_ != nullptr && watchdog_->expired;
	}

	auto lua_host::create( lua_host_options options ) -> result< lua_host > {
		auto host = lua_host{ };
		host.extension_name_ = std::move( options.extension_name );
		host.time_limit_ = options.time_limit;
		host.allocator_ = std::make_unique< detail::allocator_state >( );
		host.watchdog_ = std::make_unique< detail::watchdog_state >( );
		host.host_functions_ = std::make_unique< std::map< std::string, host_function, std::less<> > >( );

		host.allocator_->limit = options.memory_limit_bytes;

		auto* state = lua_newstate( luau_allocator, host.allocator_.get( ) );

		if ( state == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "lua_newstate failed" ) );
		}

		host.state_ = state;

		lua_pushlightuserdata( state, &g_allocator_key );
		lua_pushlightuserdata( state, host.allocator_.get( ) );
		lua_rawset( state, LUA_REGISTRYINDEX );

		lua_pushlightuserdata( state, &g_watchdog_key );
		lua_pushlightuserdata( state, host.watchdog_.get( ) );
		lua_rawset( state, LUA_REGISTRYINDEX );

		// Must be set before any extension code runs: the interrupt is the only
		// mechanism that can stop a runaway script.
		lua_callbacks( state )->interrupt = interrupt;

		luaL_openlibs( state );

		lua_newtable( state );
		lua_setglobal( state, "mcode" );

		return host;
	}

	auto lua_host::register_host_function( const std::string_view name, host_function function ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		if ( sealed_ ) {
			return std::unexpected( fail( errc::config,
				"cannot register '" + std::string{ name } + "' after the API surface is sealed" ) );
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

		lua_pushlightuserdata( state_, &entry->second );
		lua_pushcclosurek( state_, host_function_dispatch, key.c_str( ), 1, nullptr );
		lua_setfield( state_, -2, key.c_str( ) );
		lua_pop( state_, 1 );

		return { };
	}

	auto lua_host::set_global_string( const std::string_view name, const std::string_view text ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		if ( sealed_ ) {
			return std::unexpected( fail( errc::config,
				"cannot set global '" + std::string{ name } + "' after the API surface is sealed" ) );
		}

		const auto key = std::string{ name };

		lua_pushlstring( state_, text.data( ), text.size( ) );
		lua_setglobal( state_, key.c_str( ) );

		return { };
	}

	auto lua_host::seal( ) -> status {
		if ( sealed_ ) {
			return { };
		}

		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		// Makes _G, every library table, and the string metatable readonly, and
		// marks the global environment safe so the compiler may constant-fold
		// global reads. Luau enforces readonly on every C API write path, so the
		// host has no bypass -- this is a one-way door.
		luaL_sandbox( state_ );

		lua_State* thread = lua_newthread( state_ );

		if ( thread == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "lua_newthread failed" ) );
		}

		// Holds the thread alive; lua_newthread leaves it on the parent stack and
		// an unreferenced thread is collectable.
		lua_pushvalue( state_, -1 );
		thread_index_ = lua_ref( state_, -1 );
		lua_pop( state_, 1 );

		// Gives the thread its own globals table that reads through to the frozen
		// host globals, so extension globals are private and the host surface is
		// not writable.
		luaL_sandboxthread( thread );

		lua_pop( state_, 1 );

		thread_ = thread;
		sealed_ = true;

		return { };
	}

	auto lua_host::arm_watchdog( ) -> void {
		if ( watchdog_ == nullptr ) {
			return;
		}

		watchdog_->expired = false;

		if ( time_limit_.count( ) <= 0 ) {
			watchdog_->armed = false;

			return;
		}

		watchdog_->deadline = std::chrono::steady_clock::now( ) + time_limit_;
		watchdog_->armed = true;
	}

	auto lua_host::reset_thread( ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		if ( !sealed_ ) {
			return std::unexpected( fail( errc::config, "host is not sealed" ) );
		}

		lua_unref( state_, thread_index_ );
		thread_index_ = 0;
		thread_ = nullptr;

		lua_State* thread = lua_newthread( state_ );

		if ( thread == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "lua_newthread failed" ) );
		}

		lua_pushvalue( state_, -1 );
		thread_index_ = lua_ref( state_, -1 );
		luaL_sandboxthread( thread );
		lua_pop( state_, 1 );

		thread_ = thread;

		return { };
	}

	auto lua_host::run( const std::string_view chunk, const std::string_view chunk_name ) -> status {
		if ( auto ready = seal( ); !ready ) {
			return ready;
		}

		const auto name = std::string{ chunk_name };

		auto bytecode_size = std::size_t{ 0 };
		auto* bytecode = luau_compile( chunk.data( ), chunk.size( ), nullptr, &bytecode_size );

		if ( bytecode == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "compiler ran out of memory" ) );
		}

		const auto loaded = luau_load( thread_, name.c_str( ), bytecode, bytecode_size, 0 );

		std::free( bytecode );

		if ( loaded != 0 ) {
			const auto message = pop_error( thread_ );

			return std::unexpected( fail( errc::lua_error, "compile error: " + message ) );
		}

		arm_watchdog( );

		if ( lua_pcall( thread_, 0, LUA_MULTRET, 0 ) != 0 ) {
			const auto message = pop_error( thread_ );

			return std::unexpected( fail( errc::lua_error, "runtime error: " + message ) );
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

		auto bytecode_size = std::size_t{ 0 };
		auto* bytecode = luau_compile( chunk.data( ), chunk.size( ), nullptr, &bytecode_size );

		if ( bytecode == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "compiler ran out of memory" ) );
		}

		const auto loaded = luau_load( thread_, "=(eval)", bytecode, bytecode_size, 0 );

		std::free( bytecode );

		if ( loaded != 0 ) {
			return std::unexpected( fail( errc::lua_error, "compile error: " + pop_error( thread_ ) ) );
		}

		arm_watchdog( );

		if ( lua_pcall( thread_, 0, 1, 0 ) != 0 ) {
			return std::unexpected( fail( errc::lua_error, "runtime error: " + pop_error( thread_ ) ) );
		}

		auto length = std::size_t{ 0 };
		auto* text = lua_tolstring( thread_, -1, &length );

		if ( text != nullptr ) {
			auto out = std::string{ text, length };

			lua_pop( thread_, 1 );

			return out;
		}

		// lua_tolstring only converts strings and numbers. Booleans and nil --
		// which is what most probes return -- have to go through the VM's own
		// tostring, or the caller silently receives a placeholder.
		lua_getglobal( thread_, "tostring" );
		lua_pushvalue( thread_, -2 );

		if ( lua_pcall( thread_, 1, 1, 0 ) != 0 ) {
			const auto message = pop_error( thread_ );

			lua_pop( thread_, 1 );

			return std::unexpected( fail( errc::lua_error, "tostring failed: " + message ) );
		}

		text = lua_tolstring( thread_, -1, &length );

		auto out = ( text != nullptr ) ? std::string{ text, length } : std::string{ };

		lua_pop( thread_, 2 );

		return out;
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
