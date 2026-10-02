#pragma once

#include "mcode/perm/approval.hxx"

namespace mcode::perm {

	// nothing defaults to allow: an unrecognised answer re-prompts, and EOF is a deny.
	class terminal_approval_source final : public approval_source {
	public:
		[[nodiscard]] auto ask( const approval_request& request,
			const std::function< std::string( ) >& detail ) -> approval_outcome override;
	};

}

