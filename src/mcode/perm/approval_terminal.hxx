#pragma once

#include "mcode/perm/approval.hxx"

namespace mcode::perm {

	// The interactive prompt, modelled on `prompt_and_read` in exec_tools:
	// plain stdout, one line from stdin. Nothing defaults to allow -- an
	// unrecognised answer re-prompts, and `getline` failing (EOF, closed
	// stdin) is a deny.
	//
	//   mcode wants to run:
	//       git push --force origin main
	//     rule: session: ask
	//
	//     [y] allow once   [a] always allow   [n] deny once
	//     [d] never allow (this session)   [?] details
	//
	class terminal_approval_source final : public approval_source {
	public:
		[[nodiscard]] auto ask( const approval_request& request,
			const std::function< std::string( ) >& detail ) -> approval_outcome override;
	};

}

