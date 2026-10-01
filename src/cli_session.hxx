#pragma once

#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/perm/approval.hxx"

namespace mcode::cli {

	// The platform this binary was built for, as the loop reports it. Declared
	// once because the headless and the interactive path both name it, and a
	// second copy would drift.
#if defined( _WIN32 )
	inline constexpr std::string_view PLATFORM_NAME = "windows";
#elif defined( __APPLE__ )
	inline constexpr std::string_view PLATFORM_NAME = "macos";
#else
	inline constexpr std::string_view PLATFORM_NAME = "linux";
#endif

}

// Builds the loop the interactive session drives: the same construction order
// `exec` uses, in its own unit because it is the part that has to match step
// for step.
[[nodiscard]] auto build_interactive_loop( const mcode::cli::exec_options& parsed,
	mcode::perm::approval_source* interactive_approval )
	-> mcode::result< mcode::agent_loop >;

// `mcode exec [options] [prompt]` -- the headless surface.
[[nodiscard]] auto run_exec( const std::vector< std::string >& arguments ) -> int;

// The no-subcommand surface: the interactive session.
[[nodiscard]] auto run_repl( const std::vector< std::string >& arguments ) -> int;
