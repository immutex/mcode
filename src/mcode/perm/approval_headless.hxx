#pragma once

#include "mcode/perm/approval.hxx"

namespace mcode::perm {

	// fail-closed: an ask resolves to refused, which the engine resolves as deny.
	class headless_approval_source final : public approval_source {
	public:
		[[nodiscard]] auto ask( const approval_request& request,
			const std::function< std::string( ) >& detail ) -> approval_outcome override;
	};

}

