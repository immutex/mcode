#pragma once

namespace mcode {
	class agent_loop;
}

namespace mcode::cli {

	// The loop the current session is running. The extension surface is installed
	// before the loop exists and the loop is handed back by value, so a collaborator
	// that needs it resolves it here at call time rather than capturing an address
	// that does not exist yet.
	auto set_active_loop( mcode::agent_loop* loop ) -> void;

	[[nodiscard]] auto active_loop( ) -> mcode::agent_loop*;

}
