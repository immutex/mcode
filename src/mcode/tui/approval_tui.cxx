#include "mcode/tui/approval_tui.hxx"

#include <cstddef>
#include <string_view>
#include <utility>

namespace mcode::tui {

	namespace {

		inline constexpr std::string_view ANSWER_HINT = "  (y/a/n/d, or ? for details)";

		[[nodiscard]] auto is_space( const char byte ) -> bool {
			return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
		}

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

		[[nodiscard]] auto folded( const std::string& value ) -> std::string {
			return lowered( trimmed( value ) );
		}

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

		// A block left up suppresses the repaint pump, so every path out dismisses.
		const auto finish = [ & ]( const perm::approval_outcome outcome ) {
			if ( dismiss_ ) {
				dismiss_( );
			}

			return outcome;
		};

		auto notice = std::string{ };
		auto above = std::string{ };

		while ( true ) {
			// Re-presented on every pass: a redraw that shows nothing looks like a hang.
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

			// Closed input is `refused`, never a guess; the engine resolves it as deny.
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

				// A detail callback that returns nothing must still show
				// something, or `?` looks like a no-op.
				if ( above.empty( ) ) {
					notice = std::string{ "no further detail for this request" };
				}

				continue;
			}

			notice = rejection( *answer );
		}

		return finish( perm::approval_outcome::refused );
	}

}
