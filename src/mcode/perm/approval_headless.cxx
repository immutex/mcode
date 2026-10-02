#include "mcode/perm/approval_headless.hxx"

namespace mcode::perm {

	auto headless_approval_source::ask( const approval_request& request,
		const std::function< std::string( ) >& detail ) -> approval_outcome {
		std::ignore = request;
		std::ignore = detail;

		return approval_outcome::refused;
	}

}

