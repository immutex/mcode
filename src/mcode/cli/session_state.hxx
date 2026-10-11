#pragma once

#include <functional>

#include "mcode/agent/loop.hxx"
#include "mcode/ext/api_session.hxx"

namespace mcode::cli {

	// A `session_state` provider over an event log: the session's own id and sequence,
	// and the step count from the budget. Both paths hand an extension the same view,
	// so they cannot disagree about what the session is. The log must outlive it.
	[[nodiscard]] auto session_state_provider( const mcode::event_log& log,
		const mcode::session_budget& budget )
		-> std::function< mcode::ext::session_state( ) >;

}
