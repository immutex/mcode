#include "mcode/tui/render.hxx"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include "mcode/tui/frame.hxx"
#include "mcode/tui/theme.hxx"

namespace mcode::tui {

	namespace {

		inline constexpr std::array< std::string_view, SPINNER_FRAMES > SPINNER_GLYPHS = {
			"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
		};

		// One column wide, so every gutter lines up.
		inline constexpr std::string_view GUTTER_THOUGHT = "✻";
		inline constexpr std::string_view GUTTER_DONE = "✓";
		inline constexpr std::string_view GUTTER_OUTPUT = "│";

		// Measured over every tool, not the visible ones, so the target column
		// does not shift as calls come and go.
		[[nodiscard]] auto widest_verb( const std::vector< render_state::active_tool >& tools )
			-> std::size_t {
			auto widest = std::size_t{ 0 };

			for ( const auto& tool : tools ) {
				widest = std::max( widest, string_width( tool.verb, AMBIGUOUS_WIDTH ) );
			}

			return widest;
		}

		[[nodiscard]] auto widest_command_name( const std::vector< slash_command >& matches )
			-> std::size_t {
			auto widest = std::size_t{ 0 };

			for ( const auto& entry : matches ) {
				widest = std::max( widest, string_width( entry.name, AMBIGUOUS_WIDTH ) );
			}

			return widest;
		}

		// A call faster than this rounds to 0.0s, which is noise on the row.
		inline constexpr std::uint64_t MIN_SHOWN_ELAPSED_MS = 100;

		[[nodiscard]] auto format_elapsed( const std::uint64_t ms ) -> std::string {
			return std::to_string( ms / 1000 ) + "." + std::to_string( ( ms % 1000 ) / 100 ) + "s";
		}

		[[nodiscard]] auto format_tokens( const std::uint64_t tokens ) -> std::string {
			if ( tokens < 1000 ) {
				return std::to_string( tokens ) + " tok";
			}

			return std::to_string( tokens / 1000 ) + "." +
				std::to_string( ( tokens % 1000 ) / 100 ) + "k tok";
		}

		[[nodiscard]] auto format_cost( const double cost ) -> std::string {
			// A sub-millicent spend rounded to "$0.000" reads as free.
			if ( cost > 0.0 && cost < 0.001 ) {
				auto scaled = static_cast< long long >( cost * 100'000.0 );
				auto out = std::string{ "$0." };
				const auto digits = scaled < 10 ? 4 : scaled < 100 ? 3 : scaled < 1000 ? 2
					: scaled < 10'000 ? 1 : 0;
				out.append( static_cast< std::size_t >( digits ), '0' );
				out += std::to_string( scaled );

				return out;
			}

			auto whole = static_cast< long long >( cost * 1000.0 );
			auto fraction = whole % 1000;
			auto out = std::string{ "$" } + std::to_string( whole / 1000 ) + ".";

			if ( fraction < 100 ) {
				out += '0';
			}

			if ( fraction < 10 ) {
				out += '0';
			}

			return out + std::to_string( fraction );
		}

		// The live row shows only the tail, so a long chain of thought cannot
		// grow the region without bound.
		[[nodiscard]] auto tail_of( const std::string& text ) -> std::string_view {
			const auto last_break = text.rfind( '\n' );

			return last_break == std::string::npos
				? std::string_view{ text }
				: std::string_view{ text }.substr( last_break + 1 );
		}

		// Completion collapses the reasoning to one committed line, like every
		// other live row. Committing the whole chain of thought would dump
		// dozens of lines into scrollback per turn.
		[[nodiscard]] auto thought_summary( const std::string& text, const std::size_t columns )
			-> std::string {
			const auto break_at = text.find( '\n' );
			const auto first_line = std::string_view{ text }.substr( 0, break_at );
			const auto gutter = string_width( GUTTER_THOUGHT, AMBIGUOUS_WIDTH ) + 1;
			const auto budget = columns > gutter ? columns - gutter : columns;
			const auto bounded = truncate_to_width( first_line, budget, AMBIGUOUS_WIDTH );

			// Reserve the final column for the marker, or the line overruns.
			if ( bounded.size( ) == first_line.size( ) || budget == 0 ) {
				return bounded;
			}

			return truncate_to_width( first_line, budget - 1, AMBIGUOUS_WIDTH ) + "…";
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
		: previous_( LIVE_REGION_ROWS, 80 ), current_( LIVE_REGION_ROWS, 80 ) { }

	auto render_coordinator::set_capabilities( const capabilities& value ) -> void {
		caps_ = value;
	}

	auto render_coordinator::resize( const std::size_t screen_rows,
		const std::size_t screen_columns ) -> void {
		screen_rows_ = screen_rows;

		// One column is reserved: a row filled to the last column wraps the
		// cursor, which desyncs every relative move that follows.
		screen_columns_ = screen_columns > 1 ? screen_columns - 1 : screen_columns;

		previous_.resize( painted_rows_, screen_columns_ );
		current_.resize( painted_rows_, screen_columns_ );
	}

	auto render_coordinator::park( ) const -> std::string {
		return "\x1b[" + std::to_string( screen_rows_ ) + ";1H";
	}

	auto render_coordinator::invalidate( ) -> void {
		previous_.clear( );
	}

	auto render_coordinator::reserve( ) const -> std::string {
		if ( screen_rows_ <= 1 ) {
			return { };
		}

		// Scroll until the cursor is on the last row, so the region sits at the
		// bottom rather than the top.
		auto out = std::string{ };

		for ( auto index = std::size_t{ 0 }; index + 1 < screen_rows_; ++index ) {
			out += '\n';
		}

		return out;
	}

	auto render_coordinator::set_prompt( std::string text, const std::size_t cursor_byte ) -> void {
		const auto clamped = std::min( cursor_byte, text.size( ) );
		state_.input_cursor = string_width( std::string_view{ text }.substr( 0, clamped ),
			AMBIGUOUS_WIDTH );
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

	auto render_coordinator::queue_block( std::vector< styled_line > lines ) -> void {
		if ( lines.empty( ) ) {
			return;
		}

		if ( transcript_started_ ) {
			state_.pending_commit.push_back( styled_line{ } );
		}

		queue_rows( std::move( lines ) );
	}

	auto render_coordinator::queue_rows( std::vector< styled_line > lines ) -> void {
		transcript_started_ = true;

		for ( auto& line : lines ) {
			state_.pending_commit.push_back( std::move( line ) );
		}
	}

	auto render_coordinator::queue_text( const std::string_view text, const token color ) -> void {
		// A trailing newline ends the block rather than opening a blank row.
		auto body = text;

		if ( !body.empty( ) && body.back( ) == '\n' ) {
			body.remove_suffix( 1 );
		}

		if ( body.empty( ) ) {
			return;
		}

		queue_block( std::vector< styled_line >{ styled_line{
			{ std::string{ body }, color } } } );
	}

	auto render_coordinator::queue_thought( ) -> void {
		if ( state_.thinking_text.empty( ) ) {
			return;
		}

		queue_rows( std::vector< styled_line >{ styled_line{
			{ std::string{ GUTTER_THOUGHT } + " ", token::thinking },
			{ thought_summary( state_.thinking_text, screen_columns_ ), token::muted },
		} } );

		state_.thinking_text.clear( );
		state_.thinking_line.clear( );
	}

	auto render_coordinator::apply( const event_queue::item& value ) -> void {
		switch ( value.type ) {
			case event_queue::kind::thinking_delta: {
				state_.thinking_text += value.text;
				state_.thinking_line = render_inline( tail_of( state_.thinking_text ),
					token::thinking );

				break;
			}

			case event_queue::kind::assistant_delta: {
				state_.streaming_text += value.text;
				state_.streaming_line = render_inline( tail_of( state_.streaming_text ),
					token::text );

				break;
			}

			case event_queue::kind::tool_start: {
				queue_thought( );

				queue_text( state_.streaming_text );
				state_.streaming_text.clear( );
				state_.streaming_line.clear( );

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
						const auto verb_width = string_width( value.text, AMBIGUOUS_WIDTH );
						const auto widest = widest_verb( state_.tools );
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
				state_.thinking_line.clear( );
				state_.streaming_text.clear( );
				state_.streaming_line.clear( );
				state_.turn_elapsed_ms = 0;

				break;
			}

			case event_queue::kind::turn_end: {
				queue_thought( );

				queue_text( state_.streaming_text );

				state_.tools.clear( );
				state_.thinking_text.clear( );
				state_.thinking_line.clear( );
				state_.streaming_text.clear( );
				state_.streaming_line.clear( );

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

	auto render_coordinator::region_rows( ) const -> std::size_t {
		return region_rows_for( state_, screen_rows_ );
	}

	auto render_coordinator::flush( ) -> std::string {
		if ( !state_.pending_commit.empty( ) ) {
			return commit( std::move( state_.pending_commit ) );
		}

		const auto rows = region_rows( );

		// A height change moves every row. The terminal still shows the old
		// region, so it is erased first: clearing the tracked buffer alone
		// would diff the new frame against a blank model of a screen that is
		// not blank. Growth then scrolls, or the new rows paint over the
		// transcript above the region.
		auto scroll = std::string{ };

		if ( rows != painted_rows_ ) {
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
		}

		current_ = build_frame( state_, painted_rows_, screen_columns_, AMBIGUOUS_WIDTH );

		auto emitter = ansi_emitter{ caps_ };
		auto bytes = std::move( scroll );
		bytes += emitter.emit( previous_, current_ );

		// The caret belongs under the typed text, not where the last run ended.
		const auto prompt_row = painted_rows_ - 1;
		const auto widest = screen_columns_ == 0 ? std::size_t{ 0 } : screen_columns_ - 1;
		const auto column = std::min(
			std::size_t{ PROMPT_PREFIX_WIDTH } + state_.input_cursor, widest );

		bytes += emitter.caret( prompt_row, prompt_row, column );

		previous_ = current_;

		return bytes;
	}

	auto render_coordinator::commit( std::vector< styled_line > lines ) -> std::string {
		state_.pending_commit.clear( );

		auto emitter = ansi_emitter{ caps_ };

		// Park first: everything below is relative to the parked row.
		auto out = park();
		out += emitter.region_top( painted_rows_ );
		out += "\x1b[0J";

		// At depth `none` token_color resolves empty, so every SGR would be a
		// bare reset: the committed bytes carry no escape at all.
		const auto coloured = caps_.depth != capabilities::color_depth::none;

		for ( const auto& line : lines ) {
			// A fresh emitter per line: the pen cache would skip the SGR of a
			// line whose first span repeats the previous line's last style.
			auto pen = ansi_emitter{ caps_ };

			for ( const auto& span : line ) {
				if ( coloured ) {
					out += pen.sgr( span_style( span ) );
				}

				out += span.text;
			}

			out += coloured ? "\x1b[0m\n" : "\n";
		}

		// Scroll the committed text clear of the region, whatever its length.
		for ( auto index = std::size_t{ 0 }; index < painted_rows_; ++index ) {
			out += '\n';
		}

		out += park();

		previous_.clear( );

		return out;
	}

	auto region_rows_for( const render_state& state, const std::size_t screen_rows ) -> std::size_t {
		// prompt + status
		auto rows = std::size_t{ 2 };

		if ( state.palette.open && !state.palette.matches.empty( ) ) {
			rows += state.palette.matches.size( );
		}

		if ( !state.thinking_line.empty( ) ) {
			++rows;
		}

		if ( !state.streaming_line.empty( ) ) {
			++rows;
		}

		rows += state.tools.size( );

		// Bounded by the terminal and by the spec's budget.
		const auto ceiling = std::min( LIVE_REGION_MAX_ROWS,
			screen_rows > 1 ? screen_rows - 1 : std::size_t{ 1 } );

		return std::clamp( rows, std::size_t{ 2 }, std::max( ceiling, std::size_t{ 2 } ) );
	}

	auto build_frame( const render_state& state, const std::size_t row_count,
		const std::size_t column_count, const std::size_t ambiguous_width ) -> cell_buffer {
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

		// Bottom-up: prompt, palette, status line, then live content.
		{
			auto line = styled_line{ };
			line.push_back( { std::string{ PROMPT_PREFIX }, token::accent } );
			line.push_back( { state.input_line, token::text } );

			write_up( line );
		}

		if ( state.palette.open && !state.palette.matches.empty( ) ) {
			const auto widest = widest_command_name( state.palette.matches );

			// Reversed, because the buffer is filled from the bottom up.
			for ( auto index = state.palette.matches.size( ); index > 0; --index ) {
				const auto& entry = state.palette.matches[ index - 1 ];
				const auto selected = index - 1 == state.palette.selected;

				auto line = styled_line{ };
				line.push_back( { selected ? "▸ " : "  ",
					selected ? token::accent : token::muted } );
				line.push_back( { "/" + entry.name, selected ? token::accent : token::text,
					token::none, selected, false, false } );

				const auto padding = widest -
					string_width( entry.name, ambiguous_width ) + 2;
				line.push_back( { std::string( padding, ' ' ), token::none } );
				line.push_back( { entry.description, token::muted } );

				write_up( line );
			}
		}

		{
			auto line = styled_line{ };
			line.push_back( { state.model_name, token::accent } );
			line.push_back( { " ▸ ", token::muted } );
			line.push_back( { format_tokens( state.total_tokens ), token::warn } );
			line.push_back( { " ▸ ", token::muted } );
			line.push_back( { format_cost( state.total_cost ), token::warn } );
			line.push_back( { " ▸ ", token::muted } );
			line.push_back( { format_elapsed( state.turn_elapsed_ms ), token::muted } );

			write_up( line );
		}

		// Oldest first, so the newest call sits nearest the status line.
		{
			const auto widest = widest_verb( state.tools );

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

		if ( !state.thinking_line.empty( ) ) {
			auto line = styled_line{ };
			line.push_back( { std::string{ GUTTER_THOUGHT } + " ", token::thinking } );
			line.insert( line.end( ), state.thinking_line.begin( ), state.thinking_line.end( ) );

			write_up( line );
		}

		if ( !state.streaming_line.empty( ) ) {
			auto line = styled_line{ };
			line.push_back( { std::string{ GUTTER_OUTPUT } + " ", token::muted } );
			line.insert( line.end( ), state.streaming_line.begin( ), state.streaming_line.end( ) );

			write_up( line );
		}

		return buffer;
	}

}
