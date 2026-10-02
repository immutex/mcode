#pragma once

#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
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

[[nodiscard]] auto run_exec( const std::vector< std::string >& arguments ) -> int;

[[nodiscard]] auto run_repl( const std::vector< std::string >& arguments ) -> int;
