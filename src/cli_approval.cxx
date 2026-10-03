#include "cli_approval.hxx"

#include <cstddef>
#include <memory>
#include <mutex>
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
		std::atomic< bool >& active, std::mutex& gate,
		mcode::tui::render_coordinator& coordinator )
		-> mcode::tui::ui_approval_source {
		// The rows the block occupies right now, zero when none is drawn.
		// Shared with the reader and the dismisser: both need the exact height,
		// because a block that is not erased to its own height either ghosts or
		// eats the transcript above it.
		auto rows = std::make_shared< std::size_t >( 0 );

		// The read is the user's typing wait and holds no lock: a lock held
		// across it would stop the pump from repainting for as long as the
		// user thinks. Only the coordinator's mutation is serialized.
		auto read_answer = [ &session, &gate, &coordinator ]( ) {
			auto answer = session.read_line( ANSWER_WAIT_MS );

			const auto held = std::lock_guard< std::mutex >{ gate };

			// The reader echoed the answer and a newline. On the screen's last
			// row that echo scrolls, and the coordinator then models a screen
			// that has moved. The model and the caret are both put right
			// before anything else is drawn against them.
			coordinator.invalidate( );
			session.write( coordinator.park( ) );

			return answer;
		};

		// Every path out of `ask` ends here: an answered prompt, a denial, or
		// closed input. The success path and the failure path are the same
		// bytes, so a rejected answer hands the console back exactly as an
		// accepted one does. Leaving the block up suppresses the repaint pump
		// for the rest of the turn, which is what made a bad answer look like a
		// hung session.
		auto dismiss = [ &session, &active, &gate, &coordinator, rows ]( ) {
			const auto held = std::lock_guard< std::mutex >{ gate };
			const auto emitter = mcode::tui::ansi_emitter{ session.caps( ) };

			auto out = coordinator.park( );

			// Exactly the rows the block drew. Nothing drawn, nothing to
			// erase: a fixed count here would eat the transcript above a short
			// block, or leave the tall block's own rows behind.
			if ( *rows > 0 ) {
				out += emitter.clear_region( *rows );
			}

			out += coordinator.park( );

			session.write( emitter.synchronized( out ) );

			*rows = 0;

			// Cleared under the same lock the pump checks it under, so a
			// repaint cannot pass the gate and then race this dismissal.
			active.store( false );
		};

		auto present = [ &session, &active, &gate, &coordinator, rows ](
			const mcode::tui::ui_approval_source::prompt_view& view ) {
			const auto held = std::lock_guard< std::mutex >{ gate };

			// Set under the same lock the pump reads it under, so a repaint
			// cannot pass the gate and then race this draw.
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

			// What the block covers that already holds something: its own rows
			// from the last draw, or, on the first draw, the live region it
			// takes the place of. After a detail commit the region was
			// re-reserved blank, so there is nothing to erase then. The
			// region's height is derived per frame, never assumed from a
			// constant: it runs from two rows to the terminal's own height.
			auto replaced = std::size_t{ 0 };

			if ( !fresh ) {
				replaced = *rows > 0 ? *rows
					: mcode::tui::region_rows_for( coordinator.state( ),
						static_cast< std::size_t >( session.size( ).second ) );
			}

			auto out = coordinator.park( );

			if ( replaced > 0 ) {
				out += emitter.clear_region( replaced );
			}

			// A taller block scrolls its extra rows into existence, rather than
			// overwriting the transcript that sits above it.
			if ( wanted > replaced ) {
				out += coordinator.park( );

				for ( auto index = replaced; index < wanted; ++index ) {
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
