#pragma once

// bounded, and both fire on the loop's thread; the registry dies with its surface's VM.

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
		// the registry never calls Lua directly: the host decides the thread and the budget.
		explicit timer_registry( std::function< void( ) > pump );

		timer_registry( const timer_registry& ) = delete;
		auto operator=( const timer_registry& ) -> timer_registry& = delete;

		~timer_registry( );

		auto schedule( std::uint64_t delay_ms, bool repeating, int function_reference )
			-> mcode::result< timer_entry* >;

		auto stop( std::uint64_t identifier ) -> bool;
		auto stop_all( ) -> void;

		[[nodiscard]] auto find( std::uint64_t identifier ) -> timer_entry*;

		auto pump( ) -> std::size_t;

		// one fire per pass, so a handler that schedules another timer does not recurse here.
		[[nodiscard]] auto next_due( ) -> timer_entry*;

		auto reap( ) -> void;

		[[nodiscard]] auto active_count( ) const noexcept -> std::size_t;

	private:
		std::function< void( ) > pump_;
		std::map< std::uint64_t, timer_entry > timers_;
		std::uint64_t next_identifier_ = 1;
	};

	auto handle_timer_at( lua_State* state ) -> int;
	auto handle_timer_every( lua_State* state ) -> int;

}
