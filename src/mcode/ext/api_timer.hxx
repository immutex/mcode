#pragma once

// `mcode.timer.at` and `mcode.timer.every`.
//
// Both are bounded and both fire on the loop's thread. A timer registry is
// per-surface: when the extension's VM is discarded the registry dies with it,
// so a timer cannot fire after its VM is gone. The registry holds the closure
// by registry reference in the owning VM; the entry point returns a handle
// table whose `stop` closes over the identifier.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "mcode/core/error.hxx"

struct lua_State;

namespace mcode::ext {

	class api_surface;

	// One scheduled timer. The interval is zero for `timer.at`, which fires
	// once; `timer.every` re-arms from the completion of each fire.
	struct timer_entry {
		std::uint64_t identifier = 0;
		std::chrono::steady_clock::time_point fire_at{ };
		std::uint64_t interval_ms = 0;
		std::uint64_t fires_done = 0;
		std::uint64_t fires_limit = 0;

		// The closure, by registry reference in the owning VM.
		int function_reference = 0;
	};

	// The timers one extension scheduled. Host-anchored: the handles survive
	// garbage collection because the identifier is a number, not a userdata.
	class timer_registry {
	public:
		// The pump callback is what delivers a fire into the VM. The registry
		// never calls Lua directly: the host decides the thread and the budget.
		explicit timer_registry( std::function< void( ) > pump );

		timer_registry( const timer_registry& ) = delete;
		auto operator=( const timer_registry& ) -> timer_registry& = delete;

		~timer_registry( );

		auto schedule( std::uint64_t delay_ms, bool repeating, int function_reference )
			-> mcode::result< timer_entry* >;

		auto stop( std::uint64_t identifier ) -> bool;
		auto stop_all( ) -> void;

		[[nodiscard]] auto find( std::uint64_t identifier ) -> timer_entry*;

		// Fires every timer due now, on the calling thread. Returns how many
		// fired. The loop calls this between iterations; nothing here spawns
		// a thread.
		auto pump( ) -> std::size_t;

		// The next timer whose fire is due, or null. The surface's pump walks
		// one fire per pass through this, so a handler that schedules another
		// timer is handled on a later pass rather than recursing.
		[[nodiscard]] auto next_due( ) -> timer_entry*;

		// Erases every timer that has consumed its fires. An every-timer that
		// hit its bound is gone, not kept as a husk the registry never sheds.
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
