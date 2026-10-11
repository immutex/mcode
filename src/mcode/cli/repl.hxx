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

	// The loop's `run()` keeps `history_` and the budget, so a follow-up turn sees
	// the first turn's tool results.
	class session {
	public:
		explicit session( agent_loop& loop ) : loop_( &loop ) { }

		[[nodiscard]] auto run_turn( const std::string_view task ) -> cli::exit_code;

		[[nodiscard]] auto history( ) const -> const std::vector< model::message >& {
			return loop_->history( );
		}

	private:
		agent_loop* loop_ = nullptr;
	};

	// `input` is injectable so tests drive the whole session without a terminal. The
	// factory's second parameter is the approval source for the interactive path.
	using loop_factory = std::function< result< agent_loop >( const exec_options&,
		perm::approval_source* ) >;

	[[nodiscard]] auto run_session( const std::vector< std::string >& arguments,
		const std::function< std::optional< std::string >( ) >& input,
		const loop_factory& factory ) -> int;


}
