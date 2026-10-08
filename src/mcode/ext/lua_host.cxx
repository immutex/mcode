#include "mcode/ext/lua_host.hxx"
#include "mcode/ext/lua_host_internal.hxx"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

namespace mcode {

	namespace {

		[[nodiscard]] auto last_segment( const std::string_view path ) -> std::string_view {
			const auto position = path.rfind( '.' );

			return position == std::string_view::npos ? path : path.substr( position + 1 );
		}

		struct setup_request {
			::mcode::detail::watchdog_state* watchdog = nullptr;
			module_loader_function* loader = nullptr;
		};

		struct seal_request {
			lua_State* state = nullptr;
			lua_State* thread = nullptr;
		};

		// both run under lua_cpcall: Luau throws on out-of-memory, and neither may escape.
		auto initialize_state( lua_State* state ) -> int {
			auto* request = static_cast< setup_request* >( lua_touserdata( state, 1 ) );

			lua_pushlightuserdata( state, &ext::detail::g_watchdog_key );
			lua_pushlightuserdata( state, request->watchdog );
			lua_rawset( state, LUA_REGISTRYINDEX );

			lua_pushlightuserdata( state, &ext::detail::g_module_loader_key );
			lua_pushlightuserdata( state, request->loader );
			lua_rawset( state, LUA_REGISTRYINDEX );

			lua_callbacks( state )->interrupt = ext::detail::interrupt;

			luaL_openlibs( state );

			lua_newtable( state );
			lua_setglobal( state, "mcode" );

			lua_newtable( state );
			lua_setfield( state, LUA_REGISTRYINDEX, "mcode.modules" );

			lua_pushcfunction( state, ext::detail::module_require, "require" );
			lua_setglobal( state, "require" );

			return 0;
		}

		auto seal_thread( lua_State* state ) -> int {
			auto* request = static_cast< seal_request* >( lua_touserdata( state, 1 ) );

			luaL_sandbox( request->state );

			request->thread = lua_newthread( request->state );

			// lua_newthread leaves the thread collectable unless it is referenced.
			lua_pushvalue( request->state, -1 );
			lua_ref( request->state, -1 );

			luaL_sandboxthread( request->thread );

			lua_pop( request->state, 1 );

			return 0;
		}

		auto luau_allocator( void* userdata, void* pointer, std::size_t old_size,
			std::size_t new_size )
			-> void* {
			auto* counters = static_cast< ::mcode::detail::allocator_state* >( userdata );

			if ( new_size == 0 ) {
				counters->bytes -= old_size;

				std::free( pointer );

				return nullptr;
			}

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

	}

	auto ext::detail::pop_error( lua_State* state ) -> std::string {
		auto length = std::size_t{ 0 };
		const auto* message = lua_tolstring( state, -1, &length );
		auto out = ( message != nullptr ) ? std::string{ message, length }
			: std::string{ "unknown Luau error" };

		lua_pop( state, 1 );

		return out;
	}

	auto ext::detail::registry_pointer( lua_State* state, void* key ) -> void* {
		lua_pushlightuserdata( state, key );
		lua_rawget( state, LUA_REGISTRYINDEX );

		auto* value = lua_touserdata( state, -1 );
		lua_pop( state, 1 );

		return value;
	}

	auto ext::detail::watchdog_from( lua_State* state ) -> ::mcode::detail::watchdog_state* {
		return static_cast< ::mcode::detail::watchdog_state* >(
			ext::detail::registry_pointer( state, &ext::detail::g_watchdog_key ) );
	}

	auto ext::detail::loader_from( lua_State* state ) -> ::mcode::module_loader_function* {
		return static_cast< ::mcode::module_loader_function* >(
			ext::detail::registry_pointer( state, &ext::detail::g_module_loader_key ) );
	}

	auto ext::detail::load_chunk( lua_State* thread, const std::string_view source,
		const std::string_view chunk_name, std::string& message ) -> bool {
		// luau_load consumes bytecode; handing it source reads byte 0 as a version and fails.
		auto bytecode_size = std::size_t{ 0 };
		auto* bytecode = luau_compile( source.data( ), source.size( ), nullptr, &bytecode_size );

		if ( bytecode == nullptr ) {
			message = "compiler ran out of memory";

			return false;
		}

		const auto name = std::string{ chunk_name };
		const auto loaded = luau_load( thread, name.c_str( ), bytecode, bytecode_size, 0 );

		std::free( bytecode );

		if ( loaded != 0 ) {
			message = ext::detail::pop_error( thread );

			return false;
		}

		return true;
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
		, sealed_( other.sealed_ )
		, time_limit_( other.time_limit_ )
		, extension_name_( std::move( other.extension_name_ ) )
		, allocator_( std::move( other.allocator_ ) )
		, watchdog_( std::move( other.watchdog_ ) )
		, host_functions_( std::move( other.host_functions_ ) )
		, module_loader_( std::move( other.module_loader_ ) ) {
		other.state_ = nullptr;
		other.thread_ = nullptr;
	}

	auto lua_host::operator=( lua_host&& other ) noexcept -> lua_host& {
		if ( this != &other ) {
			if ( state_ != nullptr ) {
				lua_close( state_ );
			}

			state_ = other.state_;
			thread_ = other.thread_;
			sealed_ = other.sealed_;
			time_limit_ = other.time_limit_;
			extension_name_ = std::move( other.extension_name_ );
			allocator_ = std::move( other.allocator_ );
			watchdog_ = std::move( other.watchdog_ );
			host_functions_ = std::move( other.host_functions_ );
			module_loader_ = std::move( other.module_loader_ );

			other.state_ = nullptr;
			other.thread_ = nullptr;
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

	auto lua_host::time_breaches( ) const noexcept -> std::uint64_t {
		return watchdog_ != nullptr ? watchdog_->breaches : 0;
	}

	auto lua_host::time_expired( ) const noexcept -> bool {
		return watchdog_ != nullptr && watchdog_->expired;
	}

	auto lua_host::create( lua_host_options options ) -> result< lua_host > {
		auto host = lua_host{ };
		host.extension_name_ = std::move( options.extension_name );
		host.time_limit_ = options.time_limit;
		host.allocator_ = std::make_unique< ::mcode::detail::allocator_state >( );
		host.watchdog_ = std::make_unique< ::mcode::detail::watchdog_state >( );
		host.host_functions_ =
			std::make_unique< std::map< std::string, host_function, std::less<> > >( );
		host.module_loader_ =
			std::make_unique< module_loader_function >( std::move( options.module_loader ) );

		host.allocator_->limit = options.memory_limit_bytes;

		auto* state = lua_newstate( luau_allocator, host.allocator_.get( ) );

		if ( state == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "lua_newstate failed" ) );
		}

		host.state_ = state;

		auto request = setup_request{ .watchdog = host.watchdog_.get( ),
			.loader = host.module_loader_.get( ) };

		if ( lua_cpcall( state, initialize_state, &request ) != 0 ) {
			// the host's destructor closes the state; the failure is reported, not fatal.
			return std::unexpected( fail( errc::lua_error,
				"VM setup failed: " + ext::detail::pop_error( state ) ) );
		}

		return host;
	}

	auto lua_host::register_host_function( const std::string_view path,
		host_function function ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		if ( sealed_ ) {
			return std::unexpected( fail( errc::config,
				"cannot register '" + std::string{ path } + "' after the API surface is sealed" ) );
		}

		if ( path.empty( ) ) {
			return std::unexpected( fail( errc::config, "host function name must not be empty" ) );
		}

		const auto key = std::string{ path };
		const auto [ entry, inserted ] = host_functions_->try_emplace( key, std::move( function ) );

		if ( !inserted ) {
			return std::unexpected( fail( errc::config, "duplicate host function '" + key + "'" ) );
		}

		if ( auto placed = push_namespace( path ); !placed ) {
			return placed;
		}

		lua_pushlightuserdata( state_, &entry->second );
		lua_pushcclosurek( state_, ext::detail::host_function_dispatch, key.c_str( ), 1, nullptr );
		lua_setfield( state_, -2, std::string{ last_segment( path ) }.c_str( ) );
		lua_pop( state_, 1 );

		return { };
	}

	auto lua_host::register_raw_function( const std::string_view path,
		const lua_CFunction function, void* upvalue ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		if ( sealed_ ) {
			return std::unexpected( fail( errc::config,
				"cannot register '" + std::string{ path } + "' after the API surface is sealed" ) );
		}

		if ( path.empty( ) || function == nullptr ) {
			return std::unexpected( fail( errc::config,
				"a raw function needs a path and a function pointer" ) );
		}

		if ( auto placed = push_namespace( path ); !placed ) {
			return placed;
		}

		lua_pushlightuserdata( state_, upvalue );
		lua_pushcclosurek( state_, function, std::string{ path }.c_str( ), 1, nullptr );
		lua_setfield( state_, -2, std::string{ last_segment( path ) }.c_str( ) );
		lua_pop( state_, 1 );

		return { };
	}

	// leaves the parent table on the stack; a segment that exists and is not a table is refused.
	auto lua_host::push_namespace( const std::string_view path ) -> status {
		lua_getglobal( state_, "mcode" );

		auto remaining = path;
		auto position = remaining.find( '.' );

		while ( position != std::string_view::npos ) {
			const auto segment = remaining.substr( 0, position );

			if ( segment.empty( ) ) {
				lua_pop( state_, 1 );

				return std::unexpected( fail( errc::config,
					"empty namespace segment in '" + std::string{ path } + "'" ) );
			}

			lua_getfield( state_, -1, std::string{ segment }.c_str( ) );

			if ( lua_isnil( state_, -1 ) ) {
				lua_pop( state_, 1 );
				lua_newtable( state_ );
				lua_pushvalue( state_, -1 );
				lua_setfield( state_, -3, std::string{ segment }.c_str( ) );
			} else if ( !lua_istable( state_, -1 ) ) {
				lua_pop( state_, 2 );

				return std::unexpected( fail( errc::config,
					"namespace '" + std::string{ segment } + "' in '" + std::string{ path } +
					"' already exists and is not a table" ) );
			}

			lua_remove( state_, -2 );

			remaining.remove_prefix( position + 1 );
			position = remaining.find( '.' );
		}

		if ( remaining.empty( ) ) {
			lua_pop( state_, 1 );

			return std::unexpected( fail( errc::config,
				"host function path '" + std::string{ path } + "' must have a final segment" ) );
		}

		return { };
	}

	auto lua_host::set_global_string( const std::string_view name,
		const std::string_view text ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		if ( sealed_ ) {
			return std::unexpected( fail( errc::config,
				"cannot set global '" + std::string{ name } +
					"' after the API surface is sealed" ) );
		}

		if ( auto placed = push_namespace( name ); !placed ) {
			return placed;
		}

		lua_pushlstring( state_, text.data( ), text.size( ) );
		lua_setfield( state_, -2, std::string{ last_segment( name ) }.c_str( ) );
		lua_pop( state_, 1 );

		return { };
	}

	auto lua_host::set_global_number( const std::string_view path, const double value ) -> status {
		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		if ( sealed_ ) {
			return std::unexpected( fail( errc::config,
				"cannot set global '" + std::string{ path } +
					"' after the API surface is sealed" ) );
		}

		if ( auto placed = push_namespace( path ); !placed ) {
			return placed;
		}

		lua_pushnumber( state_, value );
		lua_setfield( state_, -2, std::string{ last_segment( path ) }.c_str( ) );
		lua_pop( state_, 1 );

		return { };
	}

	auto lua_host::seal( ) -> status {
		if ( sealed_ ) {
			return { };
		}

		if ( state_ == nullptr ) {
			return std::unexpected( fail( errc::lua_error, "host has no lua_State" ) );
		}

		auto request = seal_request{ .state = state_ };

		if ( lua_cpcall( state_, seal_thread, &request ) != 0 ) {
			return std::unexpected( fail( errc::lua_error,
				"VM seal failed: " + ext::detail::pop_error( state_ ) ) );
		}

		thread_ = request.thread;
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

	lua_host::budget_scope::budget_scope( lua_host* host ) noexcept
		: host_( host ) {
		if ( host_ == nullptr || host_->watchdog_ == nullptr ) {
			return;
		}

		// Only the outermost scope arms: an inner dispatch keeps the deadline
		// the outer one set rather than restarting the budget.
		if ( host_->watchdog_->depth == 0 ) {
			host_->arm_watchdog( );
		}

		++host_->watchdog_->depth;
	}

	lua_host::budget_scope::~budget_scope( ) {
		if ( host_ == nullptr || host_->watchdog_ == nullptr ) {
			return;
		}

		if ( host_->watchdog_->depth > 0 ) {
			--host_->watchdog_->depth;
		}

		// Disarming from an inner scope would leave the rest of the outer
		// handler running with no interrupt, so a loop after a nested dispatch
		// would hang the thread.
		if ( host_->watchdog_->depth == 0 ) {
			host_->disarm_watchdog( );
		}
	}

	auto lua_host::disarm_watchdog( ) -> void {
		if ( watchdog_ == nullptr ) {
			return;
		}

		// a deadline left in the past would fire at the next safepoint of an unrelated call.
		watchdog_->armed = false;
	}

}
