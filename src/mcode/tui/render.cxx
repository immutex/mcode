#include "mcode/tui/render.hxx"

#include <algorithm>
#include <array>
#include <mutex>
#include <utility>

#include "mcode/tui/frame.hxx"

namespace mcode::tui {

	namespace {

		// The live region: prompt + status + streaming + tool rows, bounded.
		inline constexpr std::size_t LIVE_ROWS = 6;

		inline constexpr std::array< std::string_view, SPINNER_FRAMES > SPINNER_GLYPHS = {
			"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
		};

		[[nodiscard]] auto format_elapsed( const std::uint64_t ms ) -> std::string {
			auto seconds = ms / 1000;
			const auto tenths = ( ms % 1000 ) / 100;

			auto out = std::to_string( seconds );
			out += '.';
			out += std::to_string( tenths );
			out += 's';

			return out;
		}

		[[nodiscard]] auto format_tokens( const std::uint64_t tokens ) -> std::string {
			if ( tokens >= 1000 ) {
				auto tenths = ( tokens % 1000 ) / 100;
				auto out = std::to_string( tokens / 1000 );
				out += '.';
				out += std::to_string( tenths );
				out += "k tok";

				return out;
			}

			return std::to_string( tokens ) + " tok";
		}

		[[nodiscard]] auto format_cost( const double cost ) -> std::string {
			// Three decimals truncate a sub-millicent spend to "$0.000", which
			// reads as "free". A cheap model costs a fraction of a millicent
			// per turn, so a non-zero spend gets a digit more rather than
			// disappearing.
			if ( cost > 0.0 && cost < 0.001 ) {
				auto scaled = static_cast< long long >( cost * 100'000.0 );
				auto out = std::string{ "$0." };

				if ( scaled < 10 ) {
					out += "0000";
				} else if ( scaled < 100 ) {
					out += "000";
				} else if ( scaled < 1000 ) {
					out += "00";
				} else if ( scaled < 10'000 ) {
					out += '0';
				}

				out += std::to_string( scaled );

				return out;
			}

			auto out = std::string{ "$" };
			auto whole = static_cast< long long >( cost * 1000 );

			out += std::to_string( whole / 1000 );
			out += '.';
			const auto fraction = whole % 1000;

			if ( fraction < 100 ) {
				out += '0';
			}

			if ( fraction < 10 ) {
				out += '0';
			}

			out += std::to_string( fraction );

			return out;
		}

	}

	auto spinner_glyph( const std::size_t frame ) -> std::string_view {
		return SPINNER_GLYPHS[ frame % SPINNER_FRAMES ];
	}

	auto event_queue::push( item value ) -> void {
		auto locked = std::lock_guard< std::mutex >{ guard_ };
		items_.push_back( std::move( value ) );
	}

	auto event_queue::drain( ) -> std::vector< item > {
		auto locked = std::lock_guard< std::mutex >{ guard_ };

		auto out = std::vector< item >{ };

		while ( !items_.empty( ) ) {
			out.push_back( std::move( items_.front( ) ) );
			items_.pop_front( );
		}

		return out;
	}

	auto event_queue::empty( ) const noexcept -> bool {
		auto locked = std::lock_guard< std::mutex >{ guard_ };

		return items_.empty( );
	}

	render_coordinator::render_coordinator( )
		: previous_( LIVE_ROWS, 80 ), current_( LIVE_ROWS, 80 ) { }

	auto render_coordinator::set_capabilities( const capabilities& value ) -> void {
		caps_ = value;
	}

	auto render_coordinator::resize( const std::size_t screen_rows,
		const std::size_t screen_columns ) -> void {
		screen_rows_ = screen_rows;
		// The buffers are the region, not the screen: the transcript above it
		// lives in the terminal's own scrollback and is never repainted.
		//
		// One column is reserved. A row filled to the terminal's last column
		// makes the terminal wrap the cursor to the next line, which desyncs
		// every relative movement that follows -- and the row that wrapped is
		// the prompt's, so the caret would land a line below the text.
		const auto usable = screen_columns > 1 ? screen_columns - 1 : screen_columns;

		previous_.resize( LIVE_REGION_ROWS, usable );
		current_.resize( LIVE_REGION_ROWS, usable );
	}

	auto render_coordinator::park( ) const -> std::string {
		auto out = std::string{ "\x1b[" };
		out += std::to_string( screen_rows_ );
		out += ";1H";

		return out;
	}

	auto render_coordinator::invalidate( ) -> void {
		previous_.clear( );
	}

	auto render_coordinator::reserve( ) const -> std::string {
		// Scroll the screen until the cursor is on its last row, so the region
		// occupies the BOTTOM `LIVE_REGION_ROWS` rows. Emitting only the
		// region's height left the cursor near the top, and every frame then
		// painted over the transcript instead of below it.
		if ( screen_rows_ <= 1 ) {
			return { };
		}

		auto out = std::string{ };

		for ( auto index = std::size_t{ 0 }; index + 1 < screen_rows_; ++index ) {
			out += '\n';
		}

		return out;
	}

	auto render_coordinator::set_prompt( std::string text, const std::size_t cursor_byte ) -> void {
		const auto clamped = std::min( cursor_byte, text.size( ) );

		// Display columns, not bytes: the caret must sit under the glyph, and a
		// wide cluster advances two.
		state_.input_cursor = string_width( std::string_view{ text }.substr( 0, clamped ),
			AMBIGUOUS_WIDTH );
		state_.input_line = std::move( text );
	}

	auto render_coordinator::set_meter( std::string model_name, const std::uint64_t tokens,
		const double cost, const std::uint64_t elapsed_ms ) -> void {
		state_.model_name = std::move( model_name );
		state_.total_tokens = tokens;
		state_.total_cost = cost;
		state_.turn_elapsed_ms = elapsed_ms;
	}

	auto render_coordinator::apply( const event_queue::item& value ) -> void {
		switch ( value.type ) {
			case event_queue::kind::assistant_delta: {
				state_.streaming_text += value.text;

				// The live region has one row for the answer, so the tail is
				// what fits. The text accumulates because each delta is a
				// fragment: rendering only the newest one showed a single chunk
				// instead of an answer that grows.
				const auto last_break = state_.streaming_text.rfind( '\n' );
				const auto tail = last_break == std::string::npos
					? std::string_view{ state_.streaming_text }
					: std::string_view{ state_.streaming_text }.substr( last_break + 1 );

				state_.streaming_line = render_inline( tail, token::text );

				break;
			}

			case event_queue::kind::tool_start: {
				// Text before a tool call is a finished message. Without this
				// the preamble and the answer after the call were concatenated
				// into one run-on line.
				if ( !state_.streaming_text.empty( ) ) {
					state_.pending_commit += state_.streaming_text;
					state_.pending_commit += '\n';
					state_.streaming_text.clear( );
					state_.streaming_line.clear( );
				}

				auto tool = render_state::active_tool{ };
				tool.verb = value.text;
				tool.started_ms = value.stamp_ms;
				tool.elapsed_ms = 0;
				tool.spinner_frame = 0;

				state_.tools.push_back( std::move( tool ) );

				break;
			}

			case event_queue::kind::tool_end: {
				// The completed call goes to scrollback, where it stays. It
				// used to be pushed into a member nothing rendered, so tool
				// results were collected and then never shown at all.
				state_.pending_commit += "\u2713 ";
				state_.pending_commit += value.text;
				state_.pending_commit += '\n';

				if ( !state_.tools.empty( ) ) {
					state_.tools.pop_back( );
				}

				break;
			}

			case event_queue::kind::turn_start: {
				// A new turn starts with a clean live region. Nothing is
				// committed here: the previous turn's text was already handed
				// over by `turn_end`, and committing again would print it
				// twice.
				state_.tools.clear( );
				state_.streaming_text.clear( );
				state_.streaming_line.clear( );
				state_.turn_elapsed_ms = 0;

				break;
			}

			case event_queue::kind::turn_end: {
				// The answer moves to the commit queue rather than being
				// dropped. The live region is cleared on every repaint, so
				// discarding the text here erased the reply before it could be
				// read -- the transcript only ever showed tool activity.
				if ( !state_.streaming_text.empty( ) ) {
					state_.pending_commit += state_.streaming_text;
					state_.pending_commit += '\n';
				}

				state_.tools.clear( );
				state_.streaming_text.clear( );
				state_.streaming_line.clear( );

				break;
			}

		}
	}

	auto render_coordinator::advance_tools( const std::uint64_t now_ms ) -> void {
		for ( auto& tool : state_.tools ) {
			// A stamp of zero means the producer did not supply one, which
			// leaves the row at its initial zero rather than inventing a start.
			if ( tool.started_ms == 0 || now_ms <= tool.started_ms ) {
				continue;
			}

			tool.elapsed_ms = now_ms - tool.started_ms;
			tool.spinner_frame = static_cast< std::size_t >(
				tool.elapsed_ms / SPINNER_INTERVAL_MS );
		}
	}

	auto render_coordinator::flush( ) -> std::string {
		if ( !state_.pending_commit.empty( ) ) {
			return commit( std::move( state_.pending_commit ) );
		}

		current_ = build_frame( state_, previous_.rows( ), previous_.columns( ), AMBIGUOUS_WIDTH );

		// The first frame is DRAWN, not just recorded. Returning empty here
		// left the screen blank until some later frame differed, and at an idle
		// prompt the only thing that differs is what the user types -- so the
		// prompt, the status line and every committed row were invisible until
		// then. `resize` has already blanked `previous_` to the right size, so
		// emitting against it writes the whole frame.
		auto emitter = ansi_emitter{ caps_ };
		auto bytes = emitter.emit( previous_, current_ );

		// Leave the caret on the prompt row, under the typed text. Without this
		// the cursor stayed wherever the last changed run ended -- the status
		// line or a tool row -- so the user could not see where typing would
		// land, and the shell's own cursor flickered around the live region.
		//
		// Clamped to the row: a prompt longer than the terminal would otherwise
		// address a column past the edge and wrap.
		const auto prompt_row = previous_.rows( ) == 0 ? std::size_t{ 0 }
			: previous_.rows( ) - 1;
		const auto widest = previous_.columns( ) == 0 ? std::size_t{ 0 }
			: previous_.columns( ) - 1;
		const auto column = std::min(
			std::size_t{ PROMPT_PREFIX_WIDTH } + state_.input_cursor, widest );

		bytes += emitter.caret( prompt_row, prompt_row, column );

		previous_ = current_;

		return bytes;
	}

	auto render_coordinator::commit( std::string text ) -> std::string {
		state_.pending_commit.clear( );

		auto emitter = ansi_emitter{ caps_ };

		// Move to the region top, erase everything from there down, print the
		// finished text into scrollback, then re-reserve the region. The
		// terminal owns the committed line from here on: it scrolls naturally
		// and survives a crash, which is the point of the hybrid model.
		auto out = emitter.region_top( LIVE_REGION_ROWS );
		out += "\x1b[0J";

		// A trailing newline would leave the cursor on the row below the text,
		// so the region is re-reserved from there.
		if ( !text.empty( ) && text.back( ) != '\n' ) {
			text += '\n';
		}

		out += text;

		// Scroll the text clear of the region.
		//
		// The text was written starting at the region's TOP row, so its first
		// lines sit exactly where the region will be redrawn -- and the next
		// frame overwrote all but the last line. Scrolling by the region's full
		// height pushes every committed line above it, whatever the text's
		// length, and leaves the cursor on the screen's last row.
		for ( auto index = std::size_t{ 0 }; index < LIVE_REGION_ROWS; ++index ) {
			out += '\n';
		}

		// Position absolutely rather than counting: the scroll above may have
		// moved the cursor by less than the newlines emitted once the screen
		// stopped scrolling.
		out += "\x1b[";
		out += std::to_string( screen_rows_ );
		out += ";1H";

		// The screen no longer shows the old frame, so the next diff must be
		// against a blank buffer rather than a stale one.
		previous_ = cell_buffer{ LIVE_REGION_ROWS, previous_.columns( ), AMBIGUOUS_WIDTH };

		return out;
	}

	auto build_frame( const render_state& state, const std::size_t row_count,
		const std::size_t column_count, const std::size_t ambiguous_width ) -> cell_buffer {
		auto buffer = cell_buffer{ row_count, column_count, ambiguous_width };

		if ( row_count == 0 || column_count == 0 ) {
			return buffer;
		}

		// Bottom-up: the prompt owns the last row, the status line sits above
		// it, tool rows and the streaming tail fill upward. The prompt is
		// anchored so the cursor lands under it regardless of how many tool
		// rows are live.
		auto row = row_count;

		const auto write_up = [&]( const styled_line& line ) {
			if ( row == 0 ) {
				return;
			}

			--row;
			buffer.write_line( row, line );
		};

		// The prompt row last, so the cursor lands under it.
		{
			auto line = styled_line{ };
			line.push_back( { std::string{ PROMPT_PREFIX }, token::accent } );
			line.push_back( { state.input_line, token::text } );

			write_up( line );
		}

		// The status line carries the meter; no progress bars.
		{
			auto line = styled_line{ };
			line.push_back( { state.model_name, token::accent } );
			line.push_back( { " \u25b8 ", token::muted } );
			line.push_back( { format_tokens( state.total_tokens ), token::warn } );
			line.push_back( { " \u25b8 ", token::muted } );
			line.push_back( { format_cost( state.total_cost ), token::warn } );
			line.push_back( { " \u25b8 ", token::muted } );
			line.push_back( { format_elapsed( state.turn_elapsed_ms ), token::muted } );

			write_up( line );
		}

		// The streaming markdown tail.
		if ( !state.streaming_line.empty( ) ) {
			write_up( state.streaming_line );
		}

		// One row per active tool call, spinner first, newest nearest the
		// status line.
		for ( auto index = state.tools.size( ); index > 0; --index ) {
			const auto& tool = state.tools[ index - 1 ];

			auto line = styled_line{ };
			line.push_back( { std::string{ spinner_glyph( tool.spinner_frame ) } + " ",
				token::accent } );
			line.push_back( { tool.verb + " ", token::text } );
			line.push_back( { tool.target, token::muted } );
			line.push_back( { " " + format_elapsed( tool.elapsed_ms ), token::muted } );

			write_up( line );
		}

		return buffer;
	}


}
