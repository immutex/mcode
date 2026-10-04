#include "mcode/ext/api_timer.hxx"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "lua.h"
#include "lualib.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		inline constexpr auto MAX_TIMER_FIRES = std::uint64_t{ 1'000 };

		inline constexpr auto MAX_ACTIVE_TIMERS = std::size_t{ 256 };

	}

	timer_registry::timer_registry( actions callbacks )
		: actions_( std::move( callbacks ) ) { }

	timer_registry::~timer_registry( ) {
		clear( );
	}

	auto timer_registry::schedule( const std::uint64_t delay_ms, const bool repeating,
		const int function_reference ) -> mcode::result< timer_entry* > {
		if ( timers_.size( ) >= MAX_ACTIVE_TIMERS ) {
			return std::unexpected( mcode::fail( mcode::errc::config,
				"mcode.timer: the timer registry is full (" +
				std::to_string( MAX_ACTIVE_TIMERS ) + " active timers)" ) );
		}

		auto entry = timer_entry{ };
		entry.identifier = next_identifier_++;
		entry.fire_at = std::chrono::steady_clock::now( )
			+ std::chrono::milliseconds{ delay_ms };
		entry.interval_ms = repeating ? delay_ms : 0;
		entry.function_reference = function_reference;
		entry.fires_limit = repeating ? MAX_TIMER_FIRES : 1;

		const auto [ position, inserted ] = timers_.insert_or_assign( entry.identifier, entry );
		(void)position;

		if ( !inserted ) {
			return std::unexpected( mcode::fail( mcode::errc::config,
				"mcode.timer: a timer with this id already exists" ) );
		}

		return &timers_.at( entry.identifier );
	}

	auto timer_registry::stop( const std::uint64_t identifier ) -> void {
		const auto found = timers_.find( identifier );

		if ( found == timers_.end( ) ) {
			return;
		}

		const auto reference = found->second.function_reference;

		timers_.erase( found );

		if ( actions_.release ) {
			actions_.release( reference );
		}
	}

	auto timer_registry::pump( ) -> std::size_t {
		auto fired = std::size_t{ 0 };
		const auto now = std::chrono::steady_clock::now( );

		// collected before firing: a handler that schedules another must not extend this scan.
		auto due = std::vector< std::uint64_t >{ };

		for ( const auto& [ identifier, entry ] : timers_ ) {
			if ( entry.fires_done < entry.fires_limit && now >= entry.fire_at ) {
				due.push_back( identifier );
			}
		}

		for ( const auto identifier : due ) {
			const auto found = timers_.find( identifier );

			if ( found == timers_.end( ) ) {
				continue;
			}

			auto& entry = found->second;

			if ( entry.fires_done >= entry.fires_limit ) {
				continue;
			}

			// counted before the call: a throwing repeating timer must still consume its fire.
			++entry.fires_done;
			++fired;

			if ( entry.fires_done < entry.fires_limit ) {
				entry.fire_at = std::chrono::steady_clock::now( )
					+ std::chrono::milliseconds{ entry.interval_ms };
			}

			// read before the call: the handler may stop this timer and erase the entry.
			const auto reference = entry.function_reference;

			if ( actions_.fire ) {
				actions_.fire( reference );
			}
		}

		reap( );

		return fired;
	}

	auto timer_registry::clear( ) -> void {
		for ( const auto& pending : timers_ ) {
			if ( actions_.release ) {
				actions_.release( pending.second.function_reference );
			}
		}

		timers_.clear( );
	}

	auto timer_registry::active_count( ) const noexcept -> std::size_t {
		return timers_.size( );
	}

	auto timer_registry::reap( ) -> void {
		for ( auto iterator = timers_.begin( ); iterator != timers_.end( ); ) {
			if ( iterator->second.fires_done < iterator->second.fires_limit ) {
				++iterator;

				continue;
			}

			const auto reference = iterator->second.function_reference;

			iterator = timers_.erase( iterator );

			if ( actions_.release ) {
				actions_.release( reference );
			}
		}
	}

	namespace {

		// the surface and the id are two upvalues: the id is a number, never a pointer.
		auto lua_timer_stop( lua_State* state ) -> int {
			auto* self = surface_from( state );

			auto* registry = self->timers( );

			if ( registry == nullptr ) {
				return 0;
			}

			registry->stop( static_cast< std::uint64_t >(
				lua_tonumber( state, lua_upvalueindex( 2 ) ) ) );

			return 0;
		}

		auto install_timer( lua_State* state, api_surface* self, const bool repeating ) -> int {
			const auto delay_ms = luaL_checknumber( state, 1 );

			if ( lua_type( state, 2 ) != LUA_TFUNCTION ) {
				lua_pushliteral( state, "mcode.timer: the second argument must be a "
					"function" );
				lua_error( state );
			}

			if ( delay_ms < 0.0 ) {
				lua_pushliteral( state, "mcode.timer: the delay must not be negative" );
				lua_error( state );
			}

			auto* registry = self->timers( );

			if ( registry == nullptr ) {
				lua_pushliteral( state, "mcode.timer: no timer registry was installed "
					"with this surface" );
				lua_error( state );
			}

			lua_pushvalue( state, 2 );
			const auto reference = lua_ref( state, -1 );
			lua_pop( state, 1 );

			const auto scheduled = registry->schedule(
				static_cast< std::uint64_t >( delay_ms ), repeating, reference );

			if ( !scheduled ) {
				lua_unref( state, reference );

				lua_pushlstring( state, scheduled.error( ).msg.data( ),
					scheduled.error( ).msg.size( ) );
				lua_error( state );
			}

			const auto identifier = ( *scheduled )->identifier;

			lua_createtable( state, 0, 1 );

			lua_pushlightuserdata( state, self );
			lua_pushnumber( state, static_cast< double >( identifier ) );
			lua_pushcclosurek( state, lua_timer_stop, "stop", 2, nullptr );
			lua_setfield( state, -2, "stop" );

			return 1;
		}

	}

	auto handle_timer_at( lua_State* state ) -> int {
		auto* self = surface_from( state );

		return install_timer( state, self, false );
	}

	auto handle_timer_every( lua_State* state ) -> int {
		auto* self = surface_from( state );

		return install_timer( state, self, true );
	}

	auto api_surface::pump_timers( ) -> void {
		if ( timers_ != nullptr ) {
			timers_->pump( );
		}
	}

	auto api_surface::fire_timer( const int function_reference ) -> void {
		auto* state = host_ != nullptr ? host_->raw( ) : nullptr;

		if ( state == nullptr ) {
			return;
		}

		// budgets the call: a timer handler must not run forever on the loop's thread.
		auto budget = lua_host::budget_scope{ host_ };

		const auto depth = lua_gettop( state );

		lua_getref( state, function_reference );

		if ( lua_type( state, -1 ) != LUA_TFUNCTION ) {
			lua_settop( state, depth );

			return;
		}

		if ( lua_pcall( state, 0, 0, 0 ) != 0 ) {
			lua_settop( state, depth );

			return;
		}

		lua_settop( state, depth );
	}

	auto api_surface::release_timer( const int function_reference ) -> void {
		auto* state = host_ != nullptr ? host_->raw( ) : nullptr;

		if ( state != nullptr && function_reference != 0 ) {
			lua_unref( state, function_reference );
		}
	}

}
