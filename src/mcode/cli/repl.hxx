#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/perm/approval.hxx"

namespace mcode::cli {

	// The interactive session: consecutive turns in one process, history
	// carried across them, the remember store alive the whole time.
	//
	// The loop's `run()` resets only per-run state and keeps `history_` and
	// the budget, so a follow-up turn sees the first turn's tool results.
	// The session is a loop over turns, not a loop around exec.
	class session {
	public:
		explicit session( agent_loop& loop ) : loop_( &loop ) { }

		// One turn. Returns the exit code the turn contributes.
		[[nodiscard]] auto run_turn( const std::string_view task ) -> cli::exit_code;

		// The loop's history, for the follow-up-sees-earlier-turns assertion.
		[[nodiscard]] auto history( ) const -> const std::vector< model::message >& {
			return loop_->history( );
		}

		[[nodiscard]] auto loop( ) const noexcept -> agent_loop& { return *loop_; }

	private:
		agent_loop* loop_ = nullptr;
	};

	// The entry point `main` calls for the no-subcommand branch. Returns the
	// process exit code. `input` is injectable so tests drive the whole
	// session without a terminal; the production path passes a stdin reader.
	// `loop_factory` builds the wired loop; the REPL owns nothing it is
	// handed. The factory's second parameter is the approval source for the
	// interactive path -- the TUI passes its raw-mode prompt source, the
	// plain fallback passes null and the factory keeps the terminal source.
	using loop_factory = std::function< result< agent_loop >( const exec_options&,
		perm::approval_source* ) >;

	[[nodiscard]] auto run_session( const std::vector< std::string >& arguments,
		const std::function< std::optional< std::string >( ) >& input,
		const loop_factory& factory ) -> int;


}
