#include "mcode/perm/approval_terminal.hxx"

#include <cstdio>
#include <iostream>
#include <string>

namespace mcode::perm {

	namespace {

		auto print_prompt( const approval_request& request ) -> void {
			std::fputs( "\n[mcode asks] ", stdout );
			std::fputs( request.action.c_str( ), stdout );
			std::fputs( ":\n    ", stdout );
			std::fputs( request.subject.c_str( ), stdout );
			std::fputs( "\n  rule: ", stdout );
			std::fputs( request.rule.c_str( ), stdout );
			std::fputs( "\n\n", stdout );
			std::fputs( "  [y] allow once   [a] always allow   [n] deny once"
				"   [d] never allow (this session)   [?] details\n", stdout );
			std::fputs( "  answer: ", stdout );
			std::fflush( stdout );
		}

	}

	auto terminal_approval_source::ask( const approval_request& request,
		const std::function< std::string( ) >& detail ) -> approval_outcome {
		while ( true ) {
			print_prompt( request );

			auto line = std::string{ };

			// EOF or a closed stdin is a deny, never an allow.
			if ( !std::getline( std::cin, line ) ) {
				return approval_outcome::refused;
			}

			if ( line == "y" ) {
				return approval_outcome::allow_once;
			}

			if ( line == "a" ) {
				return approval_outcome::allow_remember;
			}

			if ( line == "n" ) {
				return approval_outcome::deny_once;
			}

			if ( line == "d" ) {
				return approval_outcome::deny_session;
			}

			if ( line == "?" ) {
				std::fputs( "\n", stdout );
				std::fputs( detail( ).c_str( ), stdout );
				std::fputs( "\n", stdout );
				std::fflush( stdout );

				continue;
			}

			std::fputs( "  unrecognised answer; y, a, n, d or ?\n", stdout );
			std::fflush( stdout );
		}
	}

}

