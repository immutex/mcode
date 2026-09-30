#include "mcode/tui/approval_tui.hxx"

#include <utility>

namespace mcode::tui {

	ui_approval_source::ui_approval_source( answer_queue& answers )
		: answers_( &answers ) { }

	ui_approval_source::ui_approval_source( answer_source source )
		: source_( std::move( source ) ) { }

	auto ui_approval_source::render( const perm::approval_request& request ) const
		-> std::vector< std::string > {
		auto rows = std::vector< std::string >{ };

		rows.push_back( "mcode wants to " + request.action + ":" );
		rows.push_back( "    " + request.subject );
		rows.push_back( "  rule: " + request.rule );
		rows.push_back( "  [y] allow once   [a] always allow   [n] deny once" );
		rows.push_back( "  [d] never allow (this session)   [?] details" );

		return rows;
	}

	auto ui_approval_source::ask( const perm::approval_request& request,
		const std::function< std::string( ) >& detail ) -> perm::approval_outcome {
		++asks_;
		std::ignore = request;

		// Closed input is `refused`, never a guess. The engine resolves it as
		// deny, which is the fail-closed contract the whole permission layer
		// rests on.
		while ( true ) {
			auto answer = std::optional< std::string >{ };

			if ( answers_ != nullptr ) {
				if ( answers_->empty( ) ) {
					break;
				}

				answer = answers_->front( );
				answers_->pop_front( );
			} else if ( source_ ) {
				answer = source_( );
			} else {
				break;
			}

			if ( !answer ) {
				break;
			}

			if ( *answer == "y" ) {
				return perm::approval_outcome::allow_once;
			}

			if ( *answer == "a" ) {
				return perm::approval_outcome::allow_remember;
			}

			if ( *answer == "n" ) {
				return perm::approval_outcome::deny_once;
			}

			if ( *answer == "d" ) {
				return perm::approval_outcome::deny_session;
			}

			if ( *answer == "?" ) {
				++details_;
				std::ignore = detail( );

				continue;
			}

			// Anything unrecognised re-prompts. No default.
			continue;
		}

		return perm::approval_outcome::refused;
	}

}
