#include "mcode/tui/render.hxx"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "mcode/tui/frame.hxx"
#include "mcode/tui/render_internal.hxx"
#include "mcode/tui/theme.hxx"
#include "mcode/tui/transcript.hxx"

namespace mcode::tui {

	namespace {

		inline constexpr std::array< std::string_view, SPINNER_FRAMES > SPINNER_GLYPHS = {
			"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
		};

		// The byte offset whose display width reaches `column`. The caret is
		// tracked as a display column, so a split taken at the raw column
		// would cut a wide cluster in half and mangle the row.
		[[nodiscard]] auto byte_offset_at_column( const std::string_view text,
			const std::size_t column, const std::size_t ambiguous_width ) -> std::size_t {
			auto width = std::size_t{ 0 };
			auto offset = std::size_t{ 0 };

			while ( offset < text.size( ) && width < column ) {
				const auto [ cluster, cluster_width ] = next_cluster( text.substr( offset ),
					ambiguous_width );

				if ( cluster.empty( ) ) {
					break;
				}

				width += cluster_width;
				offset += cluster.size( );
			}

			return offset;
		}

		// Measured over every tool, not the visible ones, so the target column
		// does not shift as calls come and go.
		[[nodiscard]] auto widest_verb( const std::vector< render_state::active_tool >& tools,
			const std::size_t ambiguous_width ) -> std::size_t {
			auto widest = std::size_t{ 0 };

			for ( const auto& tool : tools ) {
				widest = std::max( widest, string_width( tool.verb, ambiguous_width ) );
			}

			return widest;
		}

		[[nodiscard]] auto widest_command_name( const std::vector< slash_command >& matches,
			const std::size_t ambiguous_width ) -> std::size_t {
			auto widest = std::size_t{ 0 };

			for ( const auto& entry : matches ) {
				widest = std::max( widest, string_width( entry.name, ambiguous_width ) );
			}

			return widest;
		}

		// A call faster than this rounds to 0.0s, which is noise on the row.
		inline constexpr std::uint64_t MIN_SHOWN_ELAPSED_MS = 100;

		// Re-renders one streamed buffer whose rows have fallen behind its
		// text. This is the whole-buffer render the correctness model calls
		// for, done once per read instead of once per delta.
		auto refresh_one( const std::string_view text, bool& stale,
			std::vector< styled_line >& rows, const token color, std::uint64_t& renders )
			-> void {
			if ( !stale ) {
				return;
			}

			// A commit clears a buffer's text and its rows together, so an
			// empty buffer has nothing left to render.
			if ( text.empty( ) ) {
				rows.clear( );
			} else {
				rows = transcript::render_block( text, color );
				++renders;
			}

			stale = false;
		}

		// The one reader-side entry point: every path that hands out the row
		// vectors calls this first, so a stale cache is never observed. It is
		// idempotent, so calling it twice in a paint costs nothing.
		auto refresh_streamed_rows( const render_state& state ) -> void {
			refresh_one( state.thinking_text, state.thinking_stale, state.thinking_rows,
				token::thinking, state.stream_render_count );
			refresh_one( state.streaming_text, state.streaming_stale, state.streaming_rows,
				token::text, state.stream_render_count );
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
		: previous_( LIVE_REGION_MIN_ROWS, 80 ), current_( LIVE_REGION_MIN_ROWS, 80 ) { }

	auto render_coordinator::set_capabilities( const capabilities& value ) -> void {
		caps_ = value;
	}

	auto render_coordinator::resize( const std::size_t screen_rows,
		const std::size_t screen_columns ) -> void {
		const auto reserved = screen_columns > 1 ? screen_columns - 1 : screen_columns;
		const auto changed = screen_rows != screen_rows_ || reserved != screen_columns_;

		screen_rows_ = screen_rows;

		// One column is reserved: a row filled to the last column wraps the
		// cursor, which desyncs every relative move that follows.
		screen_columns_ = reserved;

		previous_.resize( painted_rows_, screen_columns_ );
		current_.resize( painted_rows_, screen_columns_ );

		// The history viewport's height is the screen's, so a resize changes
		// how far it reaches: re-clamped, or its old depth would leave blanks.
		clamp_scroll( );

		// A size change invalidates the tracked frame: the first flush after
		// it erases the region at its old height and repaints every row, so a
		// SHRINK cannot leave a row the region no longer covers on screen.
		if ( changed ) {
			resize_pending_ = true;
		}
	}

	auto render_coordinator::park( ) const -> std::string {
		return "\x1b[" + std::to_string( screen_rows_ ) + ";1H";
	}

	auto render_coordinator::invalidate( ) -> void {
		previous_.clear( );
		repaint_all_ = true;
	}

	auto render_coordinator::set_prompt( std::string text, const std::size_t cursor_byte ) -> void {
		const auto clamped = std::min( cursor_byte, text.size( ) );
		state_.input_cursor = string_width( std::string_view{ text }.substr( 0, clamped ),
			caps_.ambiguous_width );
		state_.input_line = std::move( text );
	}

	auto render_coordinator::set_palette( slash_palette value ) -> void {
		state_.palette = std::move( value );
	}

	auto render_coordinator::set_meter( std::string model_name, const std::uint64_t tokens,
		const double cost, const std::uint64_t elapsed_ms ) -> void {
		state_.model_name = std::move( model_name );
		state_.total_tokens = tokens;
		state_.total_cost = cost;
		state_.turn_elapsed_ms = elapsed_ms;
	}

	auto render_coordinator::set_context( const std::uint64_t used,
		const std::uint64_t capacity ) -> void {
		state_.context_used = used;
		state_.context_capacity = capacity;
	}

	auto render_coordinator::set_activity( const std::string_view verb ) -> void {
		state_.activity = std::string{ verb };
	}

	auto render_coordinator::apply( const event_queue::item& value ) -> void {
		switch ( value.type ) {
			// A delta appends and marks the rows stale. Re-rendering the whole
			// buffer here would be quadratic in the answer's length, on the
			// thread that also runs the agent; the next reader renders it
			// once, which is at most once per paint.
			case event_queue::kind::thinking_delta: {
				state_.thinking_text += value.text;
				state_.thinking_stale = true;

				break;
			}

			case event_queue::kind::assistant_delta: {
				state_.streaming_text += value.text;
				state_.streaming_stale = true;

				break;
			}

			case event_queue::kind::tool_start: {
				// The commit reads the rows, so they are brought current here:
				// once per closed block, not once per delta.
				refresh_streamed_rows( state_ );

				queue_thought( );

				// The model ended its prose before calling the tool, so this
				// block closes here. What streams after the call opens a new
				// one; the closed block is never committed twice.
				queue_block( std::move( state_.streaming_rows ) );
				state_.streaming_text.clear( );
				state_.streaming_rows.clear( );
				state_.streaming_stale = false;

				auto tool = render_state::active_tool{ };
				tool.verb = value.text;
				tool.target = value.target;
				tool.started_ms = value.stamp_ms;

				state_.tools.push_back( std::move( tool ) );

				break;
			}

			case event_queue::kind::tool_end: {
				auto line = styled_line{ };
				line.push_back( { std::string{ GUTTER_DONE } + " ", token::success } );
				line.push_back( { value.text, token::text } );

				if ( !state_.tools.empty( ) ) {
					const auto& tool = state_.tools.back( );

					if ( !tool.target.empty( ) ) {
						// The live row's target column, so targets line up.
						const auto verb_width = string_width( value.text, caps_.ambiguous_width );
						const auto widest = widest_verb( state_.tools, caps_.ambiguous_width );
						const auto gap = widest + 2 > verb_width ? widest + 2 - verb_width
							: std::size_t{ 0 };

						line.push_back( { std::string( gap, ' ' ), token::none } );
						line.push_back( { tool.target, token::muted } );
					}

					// An unstamped call never ran a clock, and a call that
					// rounds to 0.0s is noise on the row.
					if ( tool.started_ms != 0 && tool.elapsed_ms >= MIN_SHOWN_ELAPSED_MS ) {
						line.push_back( { "  " + format_elapsed( tool.elapsed_ms ),
							token::muted } );
					}

					state_.tools.pop_back( );
				}

				queue_rows( std::vector< styled_line >{ std::move( line ) } );

				break;
			}

			case event_queue::kind::turn_start: {
				state_.tools.clear( );
				state_.thinking_text.clear( );
				state_.thinking_rows.clear( );
				state_.thinking_stale = false;
				state_.streaming_text.clear( );
				state_.streaming_rows.clear( );
				state_.streaming_stale = false;
				state_.turn_elapsed_ms = 0;

				break;
			}

			case event_queue::kind::turn_end: {
				// As at a tool call: the rows are brought current once, then
				// committed as they stand.
				refresh_streamed_rows( state_ );

				queue_thought( );

				// Only what is still buffered: a block already closed by a
				// tool call is committed once, not again here.
				queue_block( std::move( state_.streaming_rows ) );

				state_.tools.clear( );
				state_.thinking_text.clear( );
				state_.thinking_rows.clear( );
				state_.thinking_stale = false;
				state_.streaming_text.clear( );
				state_.streaming_rows.clear( );
				state_.streaming_stale = false;

				break;
			}
		}
	}

	auto render_coordinator::advance_tools( const std::uint64_t now_ms ) -> void {
		for ( auto& tool : state_.tools ) {
			if ( tool.started_ms == 0 || now_ms <= tool.started_ms ) {
				continue;
			}

			tool.elapsed_ms = now_ms - tool.started_ms;
			tool.spinner_frame = static_cast< std::size_t >(
				tool.elapsed_ms / SPINNER_INTERVAL_MS );
		}
	}

	auto render_coordinator::state( ) const -> const render_state& {
		// The rows are a cache of the buffers, so they are brought current
		// here rather than left for the caller to notice. Every other reader
		// goes through this or through `region_rows_for`, so no path can see
		// rows that lag their buffer.
		refresh_streamed_rows( state_ );

		return state_;
	}

	auto render_coordinator::region_rows( ) const -> std::size_t {
		return region_rows_for( state_, screen_rows_ );
	}

	auto render_coordinator::flush( ) -> std::string {
		// The commit erases the region to print above it, so the region must be
		// repainted in the same flush: returning the commit alone leaves the
		// screen with no prompt row until the next event, which is what makes
		// the input appear to vanish mid-turn.
		auto out = std::string{ };

		if ( !state_.pending_commit.empty( ) ) {
			out += commit( std::move( state_.pending_commit ) );
		}

		// Sizing reads the streamed rows, so it materialises them first: the
		// one whole-buffer render a paint costs happens here, however many
		// deltas landed since the last one.
		const auto rows = region_rows( );

		// A height change moves every row. The terminal still shows the old
		// region, so it is erased first: clearing the tracked buffer alone
		// would diff the new frame against a blank model of a screen that is
		// not blank. Growth then scrolls, or the new rows paint over the
		// transcript above the region. A resize takes the same path, so a
		// SHRINK erases at the old height before repainting.
		auto scroll = std::string{ };

		if ( rows != painted_rows_ || resize_pending_ ) {
			const auto emitter = ansi_emitter{ caps_ };

			scroll = park( );
			scroll += emitter.clear_region( painted_rows_ );

			if ( rows > painted_rows_ ) {
				scroll += park( );

				for ( auto index = painted_rows_; index < rows; ++index ) {
					scroll += '\n';
				}
			}

			painted_rows_ = rows;
			previous_.resize( rows, screen_columns_ );
			current_.resize( rows, screen_columns_ );
			previous_.clear( );
			resize_pending_ = false;
		}

		current_ = build_frame( state_, painted_rows_, screen_columns_, caps_.ambiguous_width );

		auto emitter = ansi_emitter{ caps_ };
		out += scroll;
		out += emitter.emit( previous_, current_, repaint_all_ );
		repaint_all_ = false;

		// The caret belongs under the typed text, not where the last run ended.
		// While scrolled the bottom row is the history indicator rather than
		// the prompt, so the caret parks at its left edge instead of on a
		// prompt column the viewport is not showing.
		const auto prompt_row = painted_rows_ - 1;
		const auto widest = screen_columns_ == 0 ? std::size_t{ 0 } : screen_columns_ - 1;
		const auto column = state_.scroll_offset > 0 ? std::size_t{ 0 }
			: std::min( std::size_t{ PROMPT_PREFIX_WIDTH } + state_.input_cursor, widest );

		out += emitter.caret( prompt_row, prompt_row, column );

		previous_ = current_;

		return out;
	}

	auto region_rows_for( const render_state& state, const std::size_t screen_rows )
		-> std::size_t {
		// The row counts below are what this measures, so the buffers are
		// materialised first: a region sized from stale rows would lag a paint
		// behind the text. This is also the refresh the paint relies on, since
		// `flush` sizes the region before it builds the frame.
		refresh_streamed_rows( state );

		// Bounded by the terminal and by the spec's budget.
		const auto ceiling = std::max( std::min( LIVE_REGION_MAX_ROWS,
			screen_rows > 1 ? screen_rows - 1 : std::size_t{ 1 } ), LIVE_REGION_MIN_ROWS );

		// The history viewport is a pager rather than the live region: it is
		// grown to the whole available height, so a screenful of history is
		// visible instead of the live region's bounded few rows.
		if ( state.scroll_offset > 0 ) {
			return history_region_rows( screen_rows );
		}

		// prompt + status
		auto rows = LIVE_REGION_MIN_ROWS;

		if ( state.palette.open && !state.palette.matches.empty( ) ) {
			rows += state.palette.matches.size( );
		}

		rows += std::min( state.thinking_rows.size( ), LIVE_THOUGHT_MAX_ROWS );
		rows += state.streaming_rows.size( );

		rows += state.tools.size( );

		return std::clamp( rows, LIVE_REGION_MIN_ROWS, ceiling );
	}

	auto build_frame( const render_state& state, const std::size_t row_count,
		const std::size_t column_count, const std::size_t ambiguous_width ) -> cell_buffer {
		// Last line of defence for the invariant: the frame is built from the
		// same materialised rows every other reader sees. A no-op once the
		// paint's sizing pass has refreshed them.
		refresh_streamed_rows( state );

		auto buffer = cell_buffer{ row_count, column_count, ambiguous_width };

		if ( row_count == 0 || column_count == 0 ) {
			return buffer;
		}

		auto row = row_count;

		const auto write_up = [ & ]( const styled_line& line ) {
			if ( row == 0 ) {
				return;
			}

			--row;
			buffer.write_line( row, line );
		};

		// While scrolled, the region shows retained history instead of the
		// live content, grown to the available height. The prompt is not
		// reachable while scrolled, so the history view owns the whole region.
		if ( state.scroll_offset > 0 ) {
			write_history_frame( buffer, state );

			return buffer;
		}

		// Bottom-up: prompt, palette, status line, then live content.
		{
			// the completion's remainder is ghost text, spliced only at the end of the line
			auto ghost = std::string{ };

			if ( state.palette.open && !state.palette.query.empty( ) &&
				state.palette.selected < state.palette.matches.size( ) ) {
				const auto& match = state.palette.matches[ state.palette.selected ];

				if ( match.name.size( ) > state.palette.query.size( ) &&
					std::string_view{ match.name }.starts_with( state.palette.query ) ) {
					ghost = match.name.substr( state.palette.query.size( ) );
				}
			}

			const auto split = byte_offset_at_column( state.input_line, state.input_cursor,
				ambiguous_width );

			auto line = styled_line{ };
			line.push_back( { std::string{ PROMPT_PREFIX }, token::accent } );
			line.push_back( { state.input_line.substr( 0, split ), token::text } );

			if ( !ghost.empty( ) && split == state.input_line.size( ) ) {
				line.push_back( { ghost, token::muted, token::none, false, false, true } );
			}

			line.push_back( { state.input_line.substr( split ), token::text } );

			write_up( line );
		}

		if ( state.palette.open && !state.palette.matches.empty( ) ) {
			const auto widest = widest_command_name( state.palette.matches, ambiguous_width );

			// Reversed, because the buffer is filled from the bottom up.
			for ( auto index = state.palette.matches.size( ); index > 0; --index ) {
				const auto& entry = state.palette.matches[ index - 1 ];
				const auto selected = index - 1 == state.palette.selected;

				auto line = styled_line{ };
				line.push_back( { selected ? "▸ " : "  ",
					selected ? token::accent : token::muted } );
				line.push_back( { state.palette.prefix + entry.name, selected ? token::accent
					: token::text, token::none, selected, false, false } );

				// A row without a description, every mention row, has nothing
				// to align to, so the padding and its empty tail are skipped
				// rather than trailing dead width.
				if ( !entry.description.empty( ) ) {
					const auto padding = widest -
						string_width( entry.name, ambiguous_width ) + 2;
					line.push_back( { std::string( padding, ' ' ), token::none } );
					line.push_back( { entry.description, token::muted } );
				}

				write_up( line );
			}
		}

		write_status_line( buffer, row, state );

		// Oldest first, so the newest call sits nearest the status line.
		{
			const auto widest = widest_verb( state.tools, ambiguous_width );

			for ( auto index = state.tools.size( ); index > 0; --index ) {
				const auto& tool = state.tools[ index - 1 ];

				auto line = styled_line{ };
				line.push_back( { std::string{ spinner_glyph( tool.spinner_frame ) } + " ",
					token::accent } );
				line.push_back( { tool.verb, token::text } );

				if ( !tool.target.empty( ) ) {
					const auto padding = widest -
						string_width( tool.verb, ambiguous_width ) + 2;
					line.push_back( { std::string( padding, ' ' ), token::none } );
					line.push_back( { tool.target, token::muted } );
				}

				line.push_back( { "  " + format_elapsed( tool.elapsed_ms ), token::muted } );

				write_up( line );
			}
		}

		// newest row first, capped at `max_rows`: the reasoning cannot push the answer out
		const auto write_block = [ & ]( const std::vector< styled_line >& rows,
			const std::size_t max_rows, const std::string_view gutter, const token color ) {
			const auto first = rows.size( ) - std::min( rows.size( ), max_rows );

			for ( auto index = rows.size( ); index > first; --index ) {
				write_up( transcript::prefix_row( rows[ index - 1 ], gutter, color ) );
			}
		};

		write_block( state.thinking_rows, LIVE_THOUGHT_MAX_ROWS, GUTTER_THOUGHT, token::thinking );
		write_block( state.streaming_rows, state.streaming_rows.size( ), GUTTER_OUTPUT,
			token::muted );

		return buffer;
	}

}
