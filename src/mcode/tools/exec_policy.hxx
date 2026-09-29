#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcode::tools {

	// The approval policy for exec-class calls.
	//
	// This is a GATE, not a sandbox. OS-level enforcement (restricted token on
	// Windows, Landlock on Linux, Seatbelt on macOS) is deliberately deferred; the
	// docs that own the boundary say the policy layer ships first. What this class
	// guarantees is fail-closed: with no allowlist configured and no yolo flag, an
	// exec call is denied.
	//
	// Patterns match parsed argv, not raw strings, and only whole tokens: an
	// allowlist entry `git` matches `git status` but never `git-filter-repo` or
	// `git; rm`. There is no allow-by-prefix for exec-capable runners -- `sh -c`
	// and `xargs` are refused outright, because a prefix rule against them is
	// documented-bypassable.
	enum class exec_decision { allow, deny };

	struct exec_policy {
		// Allowlisted first tokens, compared whole-token, case-sensitively. An
		// empty list allows nothing.
		std::vector< std::string > allow_argv;

		// Explicitly refused first tokens. Deny wins over allow regardless of
		// order, which is what "more restrictive wins" means at one layer.
		std::vector< std::string > deny_argv;

		// The yolo flag: the user opted out of approval prompts for this session.
		bool yolo = false;

		// The decision for one command line. `argv` is the command split into
		// tokens (the tool layer parses; the policy judges), with the first token
		// the executable name.
		[[nodiscard]] auto decide( const std::vector< std::string >& argv ) const -> exec_decision;
	};

	// Splits a shell command line into tokens the way the platform shell would:
	// whitespace-separated, with double or single quotes grouping a token.
	// Metacharacters (`&&`, `|`, `;`, backticks, `$(`) make the line unparsable
	// and return nothing -- the caller must then refuse, because a compound
	// command's later subcommand was never checked.
	[[nodiscard]] auto parse_command_line( std::string_view command )
		-> std::optional< std::vector< std::string > >;

	// Wrappers whose argument is another program. The wrapper itself being
	// allowlisted says nothing about what it runs, so an exec through one is
	// refused even when the wrapper is on the allowlist.
	[[nodiscard]] auto is_exec_runner( std::string_view program ) noexcept -> bool;

}
