#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/registry.hxx"

namespace mcode::perm {

	// Splits a shell command line into tokens the way the platform shell would:
	// whitespace-separated, with double or single quotes grouping a token.
	// Metacharacters (`&&`, `|`, `;`, backticks, `$(`) make the line unparsable
	// and return nothing -- the caller must then refuse, because a compound
	// command's later subcommand was never checked.
	//
	// Ported unchanged from the old tools/exec_policy; reviewed and correct.
	[[nodiscard]] auto parse_command_line( std::string_view command )
		-> std::optional< std::vector< std::string > >;

	// Wrappers whose argument is another program. The wrapper itself being
	// allowlisted says nothing about what it runs, so an exec through one is
	// refused even when the wrapper is on the allowlist.
	[[nodiscard]] auto is_exec_runner( std::string_view program ) noexcept -> bool;

}

