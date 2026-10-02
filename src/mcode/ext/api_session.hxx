#pragma once

// a snapshot is a plain-data copy, never a live reference; `fork` is gated by `session_fork`.

#include <cstdint>
#include <string>

#include "mcode/core/error.hxx"

struct lua_State;

namespace mcode::ext {

	class api_surface;

	struct session_state {
		std::string session;
		std::uint64_t seq = 0;
		std::uint64_t steps = 0;
		std::string goal;
		bool has_goal = false;
	};

	struct fork_result {
		std::string session;
		std::uint64_t seq = 0;
	};

	auto handle_session_snapshot( lua_State* state ) -> int;
	auto handle_session_fork( lua_State* state ) -> int;

}
