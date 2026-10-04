#pragma once

// bounded, and fired by the host on the loop's thread; the registry dies with its surface's VM.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "mcode/core/error.hxx"

struct lua_State;

namespace mcode::ext {

	class api_surface;

	struct timer_entry {
		std::uint64_t identifier = 0;
		std::chrono::steady_clock::time_point fire_at{ };
		std::uint64_t interval_ms = 0;
		std::uint64_t fires_done = 0;
		std::uint64_t fires_limit = 0;

		int function_reference = 0;
	};

	// the handles survive GC because the identifier is a number, not a userdata.
	class timer_registry {
	public:
		// the registry never calls Lua: the surface supplies the call and the budget, and
		// owns the unref, so a reference is released exactly once.
		struct actions {
			std::function< void( int function_reference ) > fire;
			std::function< void( int function_reference ) > release;
		};

		explicit timer_registry( actions callbacks );

		timer_registry( const timer_registry& ) = delete;
		auto operator=( const timer_registry& ) -> timer_registry& = delete;

		~timer_registry( );

		auto schedule( std::uint64_t delay_ms, bool repeating, int function_reference )
			-> mcode::result< timer_entry* >;

		auto stop( std::uint64_t identifier ) -> void;

		// fires every due timer once, then releases the ones that have used their fires.
		auto pump( ) -> std::size_t;

		// releases every pending reference; safe only while the owning VM is alive.
		auto clear( ) -> void;

		[[nodiscard]] auto active_count( ) const noexcept -> std::size_t;

	private:
		auto reap( ) -> void;

		actions actions_;
		std::map< std::uint64_t, timer_entry > timers_;
		std::uint64_t next_identifier_ = 1;
	};

	auto handle_timer_at( lua_State* state ) -> int;
	auto handle_timer_every( lua_State* state ) -> int;

}
