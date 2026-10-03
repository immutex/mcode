#include "mcode/tui/render_internal.hxx"

#include <algorithm>
#include <string>
#include <utility>

namespace mcode::tui {

	namespace {

		// A scrolled region is never smaller than its indicator plus one row.
		inline constexpr std::size_t HISTORY_REGION_MIN_ROWS = 2;

		// The deepest offset the viewport can show. The body holds `window`
		// rows, so a buffer taller than it stops with the oldest retained row
		// on the top row -- the furthest back the retained copy reaches, and
		// no visible row is ever a blank one. A buffer shorter than the body
		// still pages into the history view, one row deep, so PageUp always
		// answers; a lone retained row has nothing above the live view to
		// page to.
		[[nodiscard]] auto scroll_limit( const std::size_t retained, const std::size_t window )
			-> std::size_t {
			if ( retained <= 1 ) {
				return 0;
			}

			const auto deepest = retained > window ? retained - window : std::size_t{ 0 };

			return std::max( deepest, std::size_t{ 1 } );
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

		// The context window's fill, e.g. "42%". Capacity zero means unknown,
		// and the caller omits the span rather than dividing by zero.
		[[nodiscard]] auto format_context( const std::uint64_t used,
			const std::uint64_t capacity ) -> std::string {
			if ( capacity == 0 ) {
				return { };
			}

			const auto percent = used >= capacity ? std::uint64_t{ 100 }
				: used * 100 / capacity;

			return std::to_string( percent ) + "%";
		}

		// The meter's colour: normal below the warn threshold, warn from 80%,
		// error from 95%.
		[[nodiscard]] auto context_token( const std::uint64_t used,
			const std::uint64_t capacity ) -> token {
			if ( capacity == 0 ) {
				return token::muted;
			}

			const auto percent = used >= capacity ? std::uint64_t{ 100 }
				: used * 100 / capacity;

			if ( percent >= CONTEXT_ERROR_PERCENT ) {
				return token::error;
			}

			if ( percent >= CONTEXT_WARN_PERCENT ) {
				return token::warn;
			}

			return token::text;
		}

	}

	auto history_region_rows( const std::size_t screen_rows ) -> std::size_t {
		const auto available = screen_rows > 1 ? screen_rows - 1 : std::size_t{ 1 };

		return std::max( available, HISTORY_REGION_MIN_ROWS );
	}

	auto format_elapsed( const std::uint64_t ms ) -> std::string {
		return std::to_string( ms / 1000 ) + "." + std::to_string( ( ms % 1000 ) / 100 ) + "s";
	}

	auto write_status_line( cell_buffer& buffer, std::size_t& row, const render_state& state )
		-> void {
		if ( row == 0 ) {
			return;
		}

		--row;

		auto line = styled_line{ };
		line.push_back( { state.model_name, token::accent } );
		line.push_back( { " ▸ ", token::muted } );
		line.push_back( { format_tokens( state.total_tokens ), token::warn } );

		// Capacity zero means unknown: the count shows, the percentage does
		// not, rather than a division by nothing.
		const auto percent = format_context( state.context_used, state.context_capacity );

		if ( !percent.empty( ) ) {
			line.push_back( { " ", token::none } );
			line.push_back( { percent, context_token( state.context_used,
				state.context_capacity ) } );
		}

		line.push_back( { " ▸ ", token::muted } );
		line.push_back( { format_cost( state.total_cost ), token::warn } );
		line.push_back( { " ▸ ", token::muted } );
		line.push_back( { format_elapsed( state.turn_elapsed_ms ), token::muted } );

		// The activity and its hint ride on the same row, so showing and
		// clearing the verb never moves the layout.
		if ( !state.activity.empty( ) ) {
			line.push_back( { " ▸ ", token::muted } );
			line.push_back( { state.activity + "...", token::accent } );
			line.push_back( { "  " + std::string{ ACTIVITY_HINT }, token::muted,
				token::none, false, false, true } );
		}

		buffer.write_line( row, line );
	}

	auto write_history_frame( cell_buffer& buffer, const render_state& state ) -> void {
		if ( buffer.rows( ) <= HISTORY_HEADER_ROWS ) {
			return;
		}

		// The window ends `offset` rows above the newest retained row and is
		// as tall as the region above the indicator. Clamping its start keeps
		// the oldest retained row on the top row rather than running off the
		// end of the buffer.
		const auto available = buffer.rows( ) - HISTORY_HEADER_ROWS;
		const auto newest = state.scrollback.size( ) - state.scroll_offset;
		const auto oldest = newest > available ? newest - available : std::size_t{ 0 };

		auto row = available;

		for ( auto index = newest; index > oldest; --index ) {
			--row;
			buffer.write_line( row, state.scrollback[ index - 1 ] );
		}

		// The header sits on the bottom row, dim: it labels the view rather
		// than competing with the history above it.
		auto line = styled_line{ };
		line.push_back( { std::string{ HISTORY_INDICATOR }, token::muted, token::none,
			false, false, true } );

		buffer.write_line( available, line );
	}

	auto render_coordinator::clamp_scroll( ) -> void {
		// The viewport stops once the oldest retained row reaches the top of
		// its body: that is the furthest back the retained buffer can show,
		// and it keeps every visible row a real one.
		state_.scroll_offset = std::min( state_.scroll_offset,
			scroll_limit( state_.scrollback.size( ),
				history_region_rows( screen_rows_ ) - HISTORY_HEADER_ROWS ) );
	}

	auto render_coordinator::scroll_by( const int rows ) -> void {
		if ( rows == 0 ) {
			return;
		}

		const auto limit = scroll_limit( state_.scrollback.size( ),
			history_region_rows( screen_rows_ ) - HISTORY_HEADER_ROWS );

		// Up moves toward history, which grows the offset; the offset counts
		// rows between the viewport's bottom and the newest retained row.
		const auto moved = static_cast< long long >( state_.scroll_offset ) -
			static_cast< long long >( rows );

		state_.scroll_offset = static_cast< std::size_t >(
			std::clamp( moved, 0LL, static_cast< long long >( limit ) ) );
	}

	auto render_coordinator::scroll_to_bottom( ) -> void {
		state_.scroll_offset = 0;
	}

	auto render_coordinator::retain( const std::vector< styled_line >& rows ) -> void {
		for ( const auto& row : rows ) {
			state_.scrollback.push_back( row );
		}

		// The offset is measured from the newest retained row, so appended
		// rows would otherwise drag the viewport toward live. Advancing it by
		// the same count keeps the viewport on the content the user chose:
		// committing while scrolled must not move the view.
		if ( state_.scroll_offset > 0 ) {
			state_.scroll_offset += rows.size( );
		}

		// Bounded: the oldest rows fall off, and the offset shifts with them
		// so the viewport keeps showing the same rows while scrolled.
		while ( state_.scrollback.size( ) > SCROLLBACK_MAX_ROWS ) {
			state_.scrollback.pop_front( );

			if ( state_.scroll_offset > 0 ) {
				--state_.scroll_offset;
			}
		}

		// No clamp here: advancing the offset by the count appended keeps the
		// window on the same rows, and that shifted offset is always within
		// the range the buffer now covers. Clamping would pull a viewport that
		// was already scrolled toward live whenever the buffer is shorter than
		// the region -- exactly the movement a commit must not cause.
	}

}
