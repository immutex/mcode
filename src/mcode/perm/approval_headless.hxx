#pragma once

#include "mcode/perm/approval.hxx"

namespace mcode::perm {

	// The fail-closed source for headless runs (`--json`, no TTY). It never
	// prompts and never reaches stdin: an `ask` resolves to `refused`, which
	// the engine resolves as deny. The run continues -- a denied call is one
	// failed tool call, not the end of it -- and the model is told why.
	class headless_approval_source final : public approval_source {
	public:
		[[nodiscard]] auto ask( const approval_request& request,
			const std::function< std::string( ) >& detail ) -> approval_outcome override;
	};

}

