#include "mcode/tui/approval_tui.hxx"

#include <cstddef>
#include <string_view>
#include <utility>

namespace mcode::tui {

	namespace {

		// Appended to every rejection. One spelling, so the user learns it once.
		inline constexpr std::string_view ANSWER_HINT = "  (y/a/n/d, or ? for details)";

		[[nodiscard]] auto is_space( const char byte ) -> bool {
			return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
		}

		// The answer without its surrounding whitespace, so " y " is the answer
		// "y". Interior whitespace is left alone: "y e s" is not an answer.
		[[nodiscard]] auto trimmed( const std::string& value ) -> std::string_view {
			auto first = std::size_t{ 0 };
			auto last = value.size( );

			while ( first < last && is_space( value[ first ] ) ) {
				++first;
			}

			while ( last > first && is_space( value[ last - 1 ] ) ) {
				--last;
			}

			return std::string_view{ value }.substr( first, last - first );
		}

		[[nodiscard]] auto lowered( const std::string_view text ) -> std::string {
			auto out = std::string{ };
			out.reserve( text.size( ) );

			for ( const auto byte : text ) {
				out.push_back( byte >= 'A' && byte <= 'Z'
					? static_cast< char >( byte - 'A' + 'a' ) : byte );
			}

			return out;
		}

		// The answer as compared: trimmed, then case-folded. The long spellings
		// are what a user types when they are not reading the option list, so
		// they are answers too.
		[[nodiscard]] auto folded( const std::string& value ) -> std::string {
			return lowered( trimmed( value ) );
		}

		// Why the last answer was not taken. It names what arrived, so a user
		// who mistyped sees their own keystrokes and not a frozen screen.
		[[nodiscard]] auto rejection( const std::string& value ) -> std::string {
			const auto typed = trimmed( value );

			if ( typed.empty( ) ) {
				return std::string{ "no answer typed" } + std::string{ ANSWER_HINT };
			}

			return "not an answer: \"" + std::string{ typed } + "\"" +
				std::string{ ANSWER_HINT };
		}

	}

	ui_approval_source::ui_approval_source( answer_queue& answers )
		: answers_( &answers ) { }

	ui_approval_source::ui_approval_source( answer_source source, presenter present,
		dismisser dismiss )
		: source_( std::move( source ) ), present_( std::move( present ) ),
		dismiss_( std::move( dismiss ) ) { }

	auto ui_approval_source::render( const perm::approval_request& request ) const
		-> std::vector< styled_line > {
		auto rows = std::vector< styled_line >{ };

		rows.push_back( styled_line{
			{ "mcode wants to " + request.action + ":", token::accent } } );
		rows.push_back( styled_line{ { "    " + request.subject, token::text } } );
		rows.push_back( styled_line{ { "  rule: " + request.rule, token::muted } } );

		// `a` is the only answer that outlives the turn, so it says where it
		// goes; `d` is the session one, and the two must not be confused.
		rows.push_back( styled_line{
			{ "  [y]", token::accent }, { " allow once   ", token::text },
			{ "[a]", token::accent },
			{ " always allow (saved to this project's store)", token::text } } );
		rows.push_back( styled_line{
			{ "  [n]", token::accent }, { " deny once   ", token::text },
			{ "[d]", token::accent }, { " never allow (this session)   ", token::text },
			{ "[?]", token::accent }, { " details", token::text } } );

		return rows;
	}

	auto ui_approval_source::ask( const perm::approval_request& request,
		const std::function< std::string( ) >& detail ) -> perm::approval_outcome {
		++asks_;

		// Every path out of here hands the console back. A block left up
		// suppresses the repaint pump, and the session then looks hung.
		const auto finish = [ & ]( const perm::approval_outcome outcome ) {
			if ( dismiss_ ) {
				dismiss_( );
			}

			return outcome;
		};

		auto notice = std::string{ };
		auto above = std::string{ };

		while ( true ) {
			// Presented on every pass, including the re-prompts: a rejected
			// answer that only redraws nothing is indistinguishable from a hang.
			if ( present_ ) {
				auto view = prompt_view{ };
				view.rows = render( request );
				view.notice = notice;
				view.above = above;

				present_( view );
			}

			notice.clear( );
			above.clear( );

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

			// Closed input is `refused`, never a guess. The engine resolves it
			// as deny, which is the contract the permission layer rests on.
			if ( !answer ) {
				break;
			}

			const auto text = folded( *answer );

			if ( text == "y" || text == "yes" ) {
				return finish( perm::approval_outcome::allow_once );
			}

			if ( text == "a" || text == "always" ) {
				return finish( perm::approval_outcome::allow_remember );
			}

			if ( text == "n" || text == "no" ) {
				return finish( perm::approval_outcome::deny_once );
			}

			if ( text == "d" || text == "never" ) {
				return finish( perm::approval_outcome::deny_session );
			}

			if ( text == "?" ) {
				++details_;
				above = detail( );

				// A callback that returns nothing must still be visible as an
				// answer, or `?` looks exactly like the broken no-op it was.
				if ( above.empty( ) ) {
					notice = std::string{ "no further detail for this request" };
				}

				continue;
			}

			// Anything unrecognised re-prompts, and says why. No default.
			notice = rejection( *answer );
		}

		return finish( perm::approval_outcome::refused );
	}

}
