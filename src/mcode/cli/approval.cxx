#include "mcode/cli/approval.hxx"

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

		// The answer row plus one blank: the reader's Enter echo scrolls the block off its anchor.
		inline constexpr std::size_t BLOCK_EXTRA_ROWS = 2;

		[[nodiscard]] auto block_rows( const std::size_t row_count ) -> std::size_t {
			return row_count + BLOCK_EXTRA_ROWS;
		}

		// One column reserved: a full row wraps the cursor and desyncs every move after it.
		[[nodiscard]] auto block_columns( const mcode::tui::tty_session& session )
			-> std::size_t {
			const auto measured = static_cast< std::size_t >( session.size( ).first );

			return measured > 1 ? measured - 1 : measured;
		}

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
		// Shared with the reader and the dismisser: the wrong height ghosts the transcript above.
		auto rows = std::make_shared< std::size_t >( 0 );

		// Holds no lock across the read: the pump would stop repainting while the user thinks.
		auto read_answer = [ &session, &gate, &coordinator ]( ) {
			auto answer = session.read_line( ANSWER_WAIT_MS );

			const auto held = std::lock_guard< std::mutex >{ gate };

			// The echo may have scrolled the screen, so the model and caret are re-parked first.
			coordinator.invalidate( );
			session.write( coordinator.park( ) );

			return answer;
		};

		// Every exit shares these bytes: a rejected answer frees the console like an accepted one.
		auto dismiss = [ &session, &active, &gate, &coordinator, rows ]( ) {
			const auto held = std::lock_guard< std::mutex >{ gate };
			const auto emitter = mcode::tui::ansi_emitter{ session.caps( ) };

			auto out = coordinator.park( );

			// A fixed count eats the transcript above a short block or leaves a tall one's rows.
			if ( *rows > 0 ) {
				out += emitter.clear_region( *rows );
			}

			out += coordinator.park( );

			session.write( emitter.synchronized( out ) );

			*rows = 0;

			// Cleared under the gate, so a repaint cannot pass it and race this dismissal.
			active.store( false );
		};

		auto present = [ &session, &active, &gate, &coordinator, rows ](
			const mcode::tui::ui_approval_source::prompt_view& view ) {
			const auto held = std::lock_guard< std::mutex >{ gate };

			// Set under the gate, so a repaint cannot pass it and race this draw.
			active.store( true );

			// Written outside `flush`, so the coordinator's tracked frame is stale.
			coordinator.invalidate( );

			auto emitter = mcode::tui::ansi_emitter{ session.caps( ) };

			// The detail belongs above the region, as the terminal approval source puts it.
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

			// Same width policy as the live region: measuring one way and painting another desyncs.
			const auto ambiguous = session.caps( ).ambiguous_width;

			auto blank = mcode::tui::cell_buffer{ wanted, block_columns( session ), ambiguous };
			auto block = mcode::tui::cell_buffer{ wanted, blank.columns( ), ambiguous };

			for ( auto index = std::size_t{ 0 }; index < lines.size( ); ++index ) {
				block.write_line( index, lines[ index ] );
			}

			block.write_line( lines.size( ), mcode::tui::styled_line{
				{ std::string{ mcode::tui::PROMPT_PREFIX }, mcode::tui::token::accent } } );

			// Derived per frame, never assumed: two rows to the terminal's own height.
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

			// A taller block scrolls its rows into existence instead of overwriting the transcript.
			if ( wanted > replaced ) {
				out += coordinator.park( );

				for ( auto index = replaced; index < wanted; ++index ) {
					out += '\n';
				}
			}

			out += emitter.emit( blank, block );

			// The caret belongs after the prompt, not on the spare row the emitter ended on.
			out += emitter.caret( wanted - 1, wanted - 2, mcode::tui::PROMPT_PREFIX_WIDTH );

			session.write( emitter.synchronized( out ) );

			*rows = wanted;
		};

		return mcode::tui::ui_approval_source{ std::move( read_answer ),
			std::move( present ), std::move( dismiss ) };
	}

}
