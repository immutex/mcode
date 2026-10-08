#include "mcode/cli/exec.hxx"

#include <filesystem>
#include <string>
#include <string_view>

#include "exec_internal.hxx"
#include "mcode/proc/process.hxx"
#include "mcode/support/time.hxx"

namespace mcode::cli {

	// Creates a git worktree beside the checkout and returns its path. The worktree is the
	// blast-radius limit: an unattended run cannot touch the user's working tree, and the
	// rollback is `git worktree remove` rather than a snapshot store.
	auto create_worktree( const std::filesystem::path& workspace_root,
		const std::string_view name ) -> result< std::filesystem::path > {
		auto git = mcode::find_executable( "git" );

		if ( !git ) {
			return std::unexpected( fail( errc::config,
				"--worktree needs git on PATH: " + git.error( ).msg ) );
		}

		// The workspace root must be the repository root itself. `git worktree add` walks up
		// to an enclosing repository, so a directory that merely sits inside one would
		// silently check out the WRONG project - and in a plain directory under a repository
		// it would appear to succeed. Asking for the top level and comparing refuses both.
		auto top = mcode::process_options{ };
		top.executable = *git;
		top.args = { "rev-parse", "--show-toplevel" };
		top.working_directory = workspace_root.string( );
		top.environment = mcode::minimal_environment( );
		top.timeout = std::chrono::milliseconds{ detail::WORKTREE_COMMAND_TIMEOUT_MS };

		auto probe = mcode::run_process( top );

		if ( !probe || probe->exit_code != 0 ) {
			return std::unexpected( fail( errc::config,
				"--worktree needs the workspace to be a git repository root, but " +
				workspace_root.string( ) + " is not inside one" ) );
		}

		auto reported = probe->stdout_text;

		while ( !reported.empty( ) && ( reported.back( ) == '\n' || reported.back( ) == '\r' ) ) {
			reported.pop_back( );
		}

		const auto canonical_root = platform::canonicalize( workspace_root );
		const auto canonical_top = platform::canonicalize( std::filesystem::path{ reported } );

		if ( !canonical_root || !canonical_top || *canonical_root != *canonical_top ) {
			return std::unexpected( fail( errc::config,
				"--worktree needs the workspace to be the repository root; the enclosing "
				"repository is " + reported + ", so a worktree of it would be the wrong "
				"project" ) );
		}

		// A short unique suffix keeps two runs from colliding without requiring the caller
		// to name each one.
		auto label = std::string{ name };

		if ( label.empty( ) ) {
			label = std::string{ detail::DEFAULT_WORKTREE_NAME };
		}

		label += "-";
		label += std::to_string( mcode::support::epoch_milliseconds( ) );

		const auto target = workspace_root / ".mcode" / "worktrees" / label;

		auto options = mcode::process_options{ };
		options.executable = *git;
		options.args = { "worktree", "add", "--detach", target.string( ) };
		options.working_directory = workspace_root.string( );
		options.environment = mcode::minimal_environment( );
		options.timeout = std::chrono::milliseconds{ detail::WORKTREE_COMMAND_TIMEOUT_MS };

		auto ran = mcode::run_process( options );

		if ( !ran ) {
			return std::unexpected( fail( errc::config,
				"could not create a worktree: " + ran.error( ).msg ) );
		}

		// A non-zero exit means git refused, and its stderr says why. Surfacing it verbatim
		// is the only way the caller learns the directory is not a repository.
		if ( ran->exit_code != 0 ) {
			auto detail = ran->stderr_text;

			while ( !detail.empty( ) && ( detail.back( ) == '\n' || detail.back( ) == '\r' ) ) {
				detail.pop_back( );
			}

			return std::unexpected( fail( errc::config,
				"git worktree add failed: " + ( detail.empty( ) ? ran->stdout_text : detail ) ) );
		}

		return target;
	}

}
