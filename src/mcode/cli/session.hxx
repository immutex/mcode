#pragma once

#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/perm/approval.hxx"

namespace mcode::cli {

	// Named by both the headless and the interactive path; a second copy would drift.
#if defined( _WIN32 )
	inline constexpr std::string_view PLATFORM_NAME = "windows";
#elif defined( __APPLE__ )
	inline constexpr std::string_view PLATFORM_NAME = "macos";
#else
	inline constexpr std::string_view PLATFORM_NAME = "linux";
#endif

}

// The construction order must match `run_exec` step for step.
[[nodiscard]] auto build_interactive_loop( const mcode::cli::exec_options& parsed,
	mcode::perm::approval_source* interactive_approval )
	-> mcode::result< mcode::agent_loop >;

// The extension load report of the session that was built, or null when none
// was. It outlives the returned loop because it lives on the session's own
// parts, not on the loop -- the loop knows four abstractions and extensions are
// not one of them. `/extensions` reads it from here rather than the loop
// carrying a pointer it has no other use for.
[[nodiscard]] auto session_extensions( ) -> const mcode::ext::load_report*;

// Switches the running session to `id`, in place. An empty id means the newest
// session for the workspace, which is what `/continue` asks for. Repoints the
// event log, rebuilds the conversation from it and restarts the budget. Returns
// the report line to show the user.
//
// In place rather than by rebuilding: the session's parts are process-static and
// a second build is refused, so the loop, its bus subscriptions, its cancel
// source and the permission engine all have to survive. Only between turns --
// the log and the history are not safe to move under a running one.
[[nodiscard]] auto adopt_session( mcode::agent_loop& loop, std::string_view id )
	-> mcode::result< std::string >;

// Starts a fresh session for the current workspace and adopts it with an empty
// history. The session being left stays on disk and is resumable.
[[nodiscard]] auto start_new_session( mcode::agent_loop& loop ) -> mcode::result< std::string >;

// The workspace's sessions, newest first, for the `/resume` picker. An error is
// the state directory being unresolvable, which is worth reporting rather than
// showing an empty list.
[[nodiscard]] auto workspace_sessions( ) -> mcode::result< std::vector< mcode::cli::session_ref > >;

[[nodiscard]] auto run_exec( const std::vector< std::string >& arguments ) -> int;

[[nodiscard]] auto run_repl( const std::vector< std::string >& arguments ) -> int;
