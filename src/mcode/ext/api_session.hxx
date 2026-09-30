#pragma once

// `mcode.session.snapshot` and `mcode.session.fork`.
//
// A snapshot is a plain-data copy of session state -- never a live reference.
// A fork branches the event log at a sequence and is gated by the
// `session_fork` permission, checked against the calling extension's own
// manifest. The log work is delegated to the session context the host
// installed; the entry point never touches the log itself.

#include <cstdint>
#include <string>

#include "mcode/core/error.hxx"

struct lua_State;

namespace mcode::ext {

	class api_surface;

	// The session state a snapshot copies. Filled by the host; the entry
	// point marshals it into a Lua table.
	struct session_state {
		std::string session;
		std::uint64_t seq = 0;
		std::uint64_t steps = 0;
		std::string goal;
		bool has_goal = false;
	};

	// The fork outcome, returned to `api.hxx`'s host-installed forker. The
	// branch gets a new session id and records the sequence it branched at.
	struct fork_result {
		std::string session;
		std::uint64_t seq = 0;
	};

	auto handle_session_snapshot( lua_State* state ) -> int;
	auto handle_session_fork( lua_State* state ) -> int;

}
