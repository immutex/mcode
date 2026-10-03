#include "cli_approval.hxx"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mcode/tui/frame.hxx"

namespace mcode::cli {

	namespace {

		inline constexpr std::uint32_t ANSWER_WAIT_MS = 600'000;

		// The answer row, plus one blank row under it. The line reader's Enter
		// echo moves the cursor down a row; without that spare row the echo
		// would scroll the screen and carry the block off its own anchor.
		inline constexpr std::size_t BLOCK_EXTRA_ROWS = 2;

		// The block is bottom-anchored: the request rows, the answer row, and
		// the spare row below it.
		[[nodiscard]] auto block_rows( const std::size_t row_count ) -> std::size_t {
			return row_count + BLOCK_EXTRA_ROWS;
		}

		// The columns a block row may use. One column is reserved, exactly as
		// the coordinator reserves it: a row filled to the last column wraps
		// the cursor, which desyncs every relative move that follows.
		[[nodiscard]] auto block_columns( const mcode::tui::tty_session& session )
			-> std::size_t {
			const auto measured = static_cast< std::size_t >( session.size( ).first );

			return measured > 1 ? measured - 1 : measured;
		}

		// The detail view arrives as plain text. One row per line, so a newline
		// is a row break and not a cursor move inside a row.
		[[nodiscard]] auto detail_rows( std::string text )
			-> std::vector< mcode::tui::styled_line > {
			if ( !text.empty( ) && text.back( ) == '\n' ) {
				text.pop_back( );
			}

			auto rows = std::vector< mcode::tui::styled_line >{ };
			auto begin = std::size_t{ 0 };

			while ( true ) {
				const auto found = text.find( '\n', begin );
				const auto stop = found == std::string::npos ? text.size( ) : found;

				rows.push_back( mcode::tui::styled_line{
					{ text.substr( begin, stop - begin ), mcode::tui::token::text } } );

				if ( found == std::string::npos ) {
					break;
				}

				begin = found + 1;
			}

			return rows;
		}

	}

	auto make_approval_source( mcode::tui::tty_session& session,
		std::atomic< bool >& active, mcode::tui::render_coordinator& coordinator )
		-> mcode::tui::ui_approval_source {
		// The rows the block occupies right now, zero when none is drawn.
		// Shared with the reader and the dismisser: both need the exact height,
		// because a block that is not erased to its own height either ghosts or
		// eats the transcript above it.
		auto rows = std::make_shared< std::size_t >( 0 );

		auto read_answer = [ &session ]( ) {
			return session.read_line( ANSWER_WAIT_MS );
		};

		// Every path out of `ask` ends here: an answered prompt, a denial, or
		// closed input. The success path and the failure path are the same
		// bytes, so a rejected answer hands the console back exactly as an
		// accepted one does. Leaving the block up suppresses the repaint pump
		// for the rest of the turn, which is what made a bad answer look like a
		// hung session.
		auto dismiss = [ &session, &active, &coordinator, rows ]( ) {
			const auto emitter = mcode::tui::ansi_emitter{ session.caps( ) };

			auto out = coordinator.park( );

			if ( *rows > 0 ) {
				out += emitter.clear_region( *rows );
			}

			out += coordinator.park( );

			session.write( emitter.synchronized( out ) );

			*rows = 0;
			active.store( false );
		};

		auto present = [ &session, &active, &coordinator, rows ](
			const mcode::tui::ui_approval_source::prompt_view& view ) {
			active.store( true );

			// Written outside `flush`, so the coordinator's tracked frame no longer matches.
			coordinator.invalidate( );

			auto emitter = mcode::tui::ansi_emitter{ session.caps( ) };

			// The detail view belongs in the transcript, which is where the
			// terminal approval source puts it too. Queueing it as its own
			// block and flushing now writes it above the region, and takes any
			// block the pump queued alongside it rather than dropping it.
			const auto fresh = !view.above.empty( );

			if ( fresh ) {
				coordinator.queue_block( detail_rows( view.above ) );
				session.write( emitter.synchronized( coordinator.flush( ) ) );
				*rows = 0;
			}

			auto lines = view.rows;

			if ( !view.notice.empty( ) ) {
				lines.insert( lines.begin( ), mcode::tui::styled_line{
					{ view.notice, mcode::tui::token::warn } } );
			}

			const auto wanted = block_rows( lines.size( ) );

			// The block is bottom-anchored, so the emitter's own geometry
			// applies: it writes from the parked row upwards.
			auto blank = mcode::tui::cell_buffer{ wanted, block_columns( session ) };
			auto block = mcode::tui::cell_buffer{ wanted, blank.columns( ) };

			for ( auto index = std::size_t{ 0 }; index < lines.size( ); ++index ) {
				block.write_line( index, lines[ index ] );
			}

			block.write_line( lines.size( ), mcode::tui::styled_line{
				{ std::string{ mcode::tui::PROMPT_PREFIX }, mcode::tui::token::accent } } );

			// Erase exactly what the last draw used. The floor is the live
			// region's own height, which the block replaces.
			const auto erase = fresh ? std::size_t{ 0 }
				: ( *rows > 0 ? *rows : mcode::tui::LIVE_REGION_ROWS );

			auto out = coordinator.park( );

			if ( erase > 0 ) {
				out += emitter.clear_region( erase );
			}

			// A taller block scrolls its extra rows into existence, rather than
			// overwriting the transcript that sits above it.
			if ( wanted > erase ) {
				out += coordinator.park( );

				for ( auto index = erase; index < wanted; ++index ) {
					out += '\n';
				}
			}

			out += emitter.emit( blank, block );

			// The caret belongs after the prompt, not on the spare row the
			// emitter finished on.
			out += emitter.caret( wanted - 1, wanted - 2, mcode::tui::PROMPT_PREFIX_WIDTH );

			session.write( emitter.synchronized( out ) );

			*rows = wanted;
		};

		return mcode::tui::ui_approval_source{ std::move( read_answer ),
			std::move( present ), std::move( dismiss ) };
	}

}
