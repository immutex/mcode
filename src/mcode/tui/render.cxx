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

	auto render_coordinator::resize( const std::size_t row_count,
		const std::size_t column_count ) -> void {
		previous_.resize( row_count, column_count );
		current_.resize( row_count, column_count );
	}

	auto render_coordinator::apply( const event_queue::item& value ) -> void {
		switch ( value.type ) {
			case event_queue::kind::assistant_delta: {
				state_.streaming_line = render_inline( value.text, token::text );

				break;
			}

			case event_queue::kind::tool_start: {
				auto tool = render_state::active_tool{ };
				tool.verb = value.text;
				tool.elapsed_ms = 0;
				tool.spinner_frame = 0;

				state_.tools.push_back( std::move( tool ) );

				break;
			}

			case event_queue::kind::tool_end: {
				// A tool result carrying a unified diff renders through the
				// diff view: line tinting plus word-level emphasis.
				if ( value.text.find( "@@ " ) != std::string::npos ) {
					for ( auto& rendered : render_diff( value.text, token::text ) ) {
						state_.committed.push_back( std::move( rendered.spans ) );
					}
				} else {
					auto line = styled_line{ };
					line.push_back( { std::string{ "✓ " }, token::success } );
					line.push_back( { value.text, token::text } );

					state_.committed.push_back( std::move( line ) );
				}

				if ( !state_.tools.empty( ) ) {
					state_.tools.pop_back( );
				}

				break;
			}

			case event_queue::kind::turn_start: {
				state_.tools.clear( );
				state_.streaming_line.clear( );
				state_.turn_elapsed_ms = 0;

				break;
			}

			case event_queue::kind::turn_end: {
				state_.tools.clear( );
				state_.streaming_line.clear( );

				break;
			}

			case event_queue::kind::usage: {
				state_.total_tokens += value.tokens;
				state_.total_cost += value.cost;

				break;
			}

			case event_queue::kind::shutdown: {
				break;
			}
		}
	}

	auto render_coordinator::flush( ) -> std::string {
		current_ = build_frame( state_, previous_.rows( ), previous_.columns( ), 1 );

		if ( first_frame_ ) {
			first_frame_ = false;

			previous_ = current_;

			return std::string{ };
		}

		auto emitter = ansi_emitter{ caps_ };
		auto bytes = emitter.emit( previous_, current_, 0 );

		previous_ = current_;

		return bytes;
	}

	auto render_coordinator::commit_streaming( ) -> std::string {
		auto emitter = ansi_emitter{ caps_ };

		auto out = emitter.clear_region( 0, previous_.rows( ) );

		if ( !state_.streaming_line.empty( ) ) {
			for ( const auto& span : state_.streaming_line ) {
				out += span.text;
			}
		}

		out += '\n';

		state_.streaming_line.clear( );

		return out;
	}

	auto render_coordinator::commit_tool( const styled_span& line ) -> std::string {
		auto emitter = ansi_emitter{ caps_ };

		auto out = emitter.clear_region( 0, previous_.rows( ) );
		out += line.text;
		out += '\n';

		state_.committed.push_back( styled_line{ line } );

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
			line.push_back( { std::string{ "> " }, token::accent } );
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
