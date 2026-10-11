#pragma once

#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/mcp/connect.hxx"
#include "mcode/perm/approval.hxx"
#include "mcode/tui/palette.hxx"

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

// Null when no session was built; lives on the session's parts, not the loop.
[[nodiscard]] auto session_extensions( ) -> const mcode::ext::load_report*;

// Builtins plus every command the loaded extensions registered. Owned by the
// session, so a palette pointer into it stays valid across a refresh.
[[nodiscard]] auto session_commands( ) -> const std::vector< mcode::tui::slash_command >*;

// Runs a command an extension registered. A name no loaded extension owns is an
// error, never a silent no-op.
[[nodiscard]] auto session_invoke_command( std::string_view name, std::string_view arguments )
	-> mcode::result< std::string >;

// The connected MCP servers, so `/prompt` can reach their prompts. Null when no
// session was built.
[[nodiscard]] auto session_mcp_servers( ) -> const mcode::mcp::server_set*;

// An empty id means the workspace's newest session. In place, because the parts
// are process-static and a second build is refused. Only between turns.
[[nodiscard]] auto adopt_session( mcode::agent_loop& loop, std::string_view id )
	-> mcode::result< std::string >;

// The session being left stays on disk and is resumable.
[[nodiscard]] auto start_new_session( mcode::agent_loop& loop ) -> mcode::result< std::string >;

// Newest first; an error is the state directory being unresolvable.
[[nodiscard]] auto workspace_sessions( ) -> mcode::result< std::vector< mcode::cli::session_ref > >;

[[nodiscard]] auto run_exec( const std::vector< std::string >& arguments ) -> int;

[[nodiscard]] auto run_repl( const std::vector< std::string >& arguments ) -> int;
