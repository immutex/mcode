#include "cli_approval.hxx"

#include <memory>
#include <string>
#include <vector>

#include "mcode/tui/frame.hxx"

namespace mcode::cli {

	namespace {

		inline constexpr std::uint32_t ANSWER_WAIT_MS = 600'000;

		// Drawn from the parked row upwards: the question plus the row the answer is typed on.
		[[nodiscard]] auto block_rows( const std::size_t row_count ) -> std::size_t {
			return row_count + 1;
		}

		// Anything else re-prompts inside `ask`, which reads again without re-presenting.
		[[nodiscard]] auto settles( const std::string& answer ) -> bool {
			return answer.size( ) == 1 && ( answer == "y" || answer == "a" ||
				answer == "n" || answer == "d" );
		}

	}

	auto make_approval_source( mcode::tui::tty_session& session,
		std::atomic< bool >& active, mcode::tui::render_coordinator& coordinator )
		-> mcode::tui::ui_approval_source {
		// Shared with the answer reader: it needs the height the presenter drew.
		auto rows = std::make_shared< std::size_t >( 0 );

		auto read_answer = [ &session, &active, &coordinator, rows ]( ) {
			auto answer = session.read_line( ANSWER_WAIT_MS );

			if ( !answer ) {
				// Leaving the block up would freeze the pump for the rest of the turn.
				active.store( false );

				return answer;
			}

			if ( !settles( *answer ) ) {
				return answer;
			}

			session.write( mcode::tui::ansi_emitter{ session.caps( ) }
				.clear_region( *rows ) + coordinator.park( ) );

			// The pump draws the streaming text, so it must resume here rather than later.
			active.store( false );

			return answer;
		};

		auto present = [ &session, &active, &coordinator, rows ](
			const std::vector< std::string >& lines ) {
			active.store( true );
			*rows = block_rows( lines.size( ) );

			// Written outside `flush`, so the coordinator's tracked frame no longer matches.
			coordinator.invalidate( );

			const auto emitter = mcode::tui::ansi_emitter{ session.caps( ) };

			auto out = coordinator.park( );
			out += emitter.clear_region( mcode::tui::LIVE_REGION_ROWS );
			out += emitter.region_top( mcode::tui::LIVE_REGION_ROWS );

			for ( const auto& line : lines ) {
				out += line;
				out += "\r\n";
			}

			out += "> ";
			session.write( out );
		};

		return mcode::tui::ui_approval_source{ std::move( read_answer ),
			std::move( present ) };
	}

}
