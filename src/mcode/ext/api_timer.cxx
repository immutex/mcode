#include "mcode/ext/api_timer.hxx"

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

#include "lua.h"
#include "lualib.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_internal.hxx"

namespace mcode::ext {

	namespace {

		// A timer that fires forever is a leak with a friendly name. The bound
		// is a named constant because it is part of the frozen surface's bound
		// table, not a tuning knob.
		inline constexpr auto MAX_TIMER_FIRES = std::uint64_t{ 1'000 };

		// The registry is bounded: an extension that schedules in a loop hits
		// the cap and gets an error rather than silently degrading every event
		// dispatch.
		inline constexpr auto MAX_ACTIVE_TIMERS = std::size_t{ 256 };

	}

	timer_registry::timer_registry( std::function< void( ) > pump )
		: pump_( std::move( pump ) ) { }

	timer_registry::~timer_registry( ) {
		// Nothing owns the closures but the VM, which dies with the extension.
		// The map's entries hold only references and a deadline.
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

		auto [ position, inserted ] = timers_.insert_or_assign( entry.identifier, entry );
		(void)position;

		if ( !inserted ) {
			return std::unexpected( mcode::fail( mcode::errc::config,
				"mcode.timer: a timer with this id already exists" ) );
		}

		return &timers_.at( entry.identifier );
	}

	auto timer_registry::stop( const std::uint64_t identifier ) -> bool {
		return timers_.erase( identifier ) > 0;
	}

	auto timer_registry::stop_all( ) -> void {
		timers_.clear( );
	}

	auto timer_registry::find( const std::uint64_t identifier ) -> timer_entry* {
		const auto found = timers_.find( identifier );

		return found != timers_.end( ) ? &found->second : nullptr;
	}

	auto timer_registry::pump( ) -> std::size_t {
		auto fired = std::size_t{ 0 };

		if ( !pump_ ) {
			return fired;
		}

		const auto now = std::chrono::steady_clock::now( );

		// Timers due now, collected first: a handler that schedules another
		// timer must not extend this iteration's scan.
		auto due = std::vector< std::uint64_t >{ };

		for ( auto& [ identifier, entry ] : timers_ ) {
			if ( entry.fires_done >= entry.fires_limit ) {
				continue;
			}

			if ( now >= entry.fire_at ) {
				due.push_back( identifier );
			}
		}

		for ( const auto identifier : due ) {
			auto* entry = find( identifier );

			if ( entry == nullptr || entry->fires_done >= entry->fires_limit ) {
				continue;
			}

			// Counted BEFORE the call: a handler that throws must still have
			// consumed its fire, or a throwing repeating timer would run
			// forever inside one pump.
			++entry->fires_done;
			++fired;

			if ( entry->fires_done < entry->fires_limit ) {
				entry->fire_at = std::chrono::steady_clock::now( )
					+ std::chrono::milliseconds{ entry->interval_ms };
			}

			pump_( );
		}

		// Drop the exhausted. An every-timer that hit its bound is gone, not
		// kept as a husk the registry can never shed.
		for ( auto iterator = timers_.begin( ); iterator != timers_.end( ); ) {
			if ( iterator->second.fires_done >= iterator->second.fires_limit ) {
				iterator = timers_.erase( iterator );
			} else {
				++iterator;
			}
		}

		return fired;
	}

	auto timer_registry::active_count( ) const noexcept -> std::size_t {
		return timers_.size( );
	}

	auto timer_registry::next_due( ) -> timer_entry* {
		const auto now = std::chrono::steady_clock::now( );

		for ( auto& [ identifier, entry ] : timers_ ) {
			if ( entry.fires_done < entry.fires_limit && now >= entry.fire_at ) {
				return &entry;
			}
		}

		return nullptr;
	}

	auto timer_registry::reap( ) -> void {
		for ( auto iterator = timers_.begin( ); iterator != timers_.end( ); ) {
			if ( iterator->second.fires_done >= iterator->second.fires_limit ) {
				iterator = timers_.erase( iterator );
			} else {
				++iterator;
			}
		}
	}

	namespace {

		// The handle table's `stop` method. The identifier travels as a
		// lightuserdata upvalue: it is a number on the host side, not a stored
		// pointer, and the registry is found through the surface so a handle
		// from a discarded VM fails closed rather than reaching into a dead map.
		auto lua_timer_stop( lua_State* state ) -> int {
			auto* self = surface_from( state );

			auto* registry = self->timers( );

			if ( registry == nullptr ) {
				return 0;
			}

			const auto* identifier = static_cast< const std::uint64_t* >(
				lua_touserdata( state, lua_upvalueindex( 1 ) ) );

			if ( identifier == nullptr ) {
				return 0;
			}

			registry->stop( *identifier );

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

			// The handle: a table with a `stop` method closing over the id. The
			// id is stored as a heap value the closure owns, so the table
			// survives the scheduling call.
			auto* stored = static_cast< std::uint64_t* >(
				std::malloc( sizeof( std::uint64_t ) ) );

			if ( stored == nullptr ) {
				registry->stop( identifier );
				lua_unref( state, reference );

				lua_pushliteral( state, "mcode.timer: out of memory" );
				lua_error( state );
			}

			*stored = identifier;

			lua_createtable( state, 0, 1 );

			lua_pushlightuserdata( state, stored );
			lua_pushcclosurek( state, lua_timer_stop, "stop", 1, nullptr );
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

}

namespace mcode::ext {

	auto api_surface::pump_timers_once( ) -> void {
		auto* state = host_ != nullptr ? host_->raw( ) : nullptr;

		if ( state == nullptr ) {
			return;
		}

		auto* registry = timers_.get( );

		if ( registry == nullptr ) {
			return;
		}

		// One fire per pump pass: the loop calls this between iterations, so a
		// handler that schedules another timer sees it on the next pass rather
		// than recursing here.
		for ( ;; ) {
			auto* due = registry->next_due( );

			if ( due == nullptr ) {
				break;
			}

			// Counted BEFORE the call: a handler that throws must still have
			// consumed its fire, or a throwing repeating timer would run
			// forever inside one pump.
			const auto reference = due->function_reference;

			if ( due->fires_done + 1 >= due->fires_limit ) {
				due->fires_done = due->fires_limit;
			} else {
				++due->fires_done;
				due->fire_at = std::chrono::steady_clock::now( )
					+ std::chrono::milliseconds{ due->interval_ms };
			}

			auto budget = lua_host::budget_scope{ host_ };

			const auto depth = lua_gettop( state );

			lua_getref( state, reference );

			if ( lua_type( state, -1 ) != LUA_TFUNCTION ) {
				lua_settop( state, depth );

				continue;
			}

			// A timer handler that throws is contained and counted, exactly as
			// a hook handler is. The fire is consumed either way.
			if ( lua_pcall( state, 0, 0, 0 ) != 0 ) {
				lua_settop( state, depth );

				continue;
			}

			lua_settop( state, depth );
		}

		// Drop the exhausted. An every-timer that hit its bound is gone, not
		// kept as a husk the registry can never shed.
		registry->reap( );
	}

}
