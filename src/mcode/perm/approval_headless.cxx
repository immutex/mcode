#include "mcode/perm/approval_headless.hxx"

namespace mcode::perm {

	auto headless_approval_source::ask( const approval_request& request,
		const std::function< std::string( ) >& detail ) -> approval_outcome {
		// No TTY means no prompt at all, not a prompt that reads EOF forever.
		// The detail callback is never called: there is nobody to show it to.
		std::ignore = request;
		std::ignore = detail;

		return approval_outcome::refused;
	}

}

