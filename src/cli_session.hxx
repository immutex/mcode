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

[[nodiscard]] auto run_exec( const std::vector< std::string >& arguments ) -> int;

[[nodiscard]] auto run_repl( const std::vector< std::string >& arguments ) -> int;
