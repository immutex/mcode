#include "mcode/tui/frame.hxx"

#include "mcode/tui/theme.hxx"

#include <array>
#include <optional>
#include <utility>

namespace mcode::tui {

	auto diff_rows( const cell_buffer& previous, const cell_buffer& current )
		-> std::vector< cell_run > {
		auto out = std::vector< cell_run >{ };

		if ( previous.rows( ) != current.rows( ) ||
			previous.columns( ) != current.columns( ) ) {
			return out;
		}

		for ( auto row = std::size_t{ 0 }; row < current.rows( ); ++row ) {
			auto run_start = std::optional< std::size_t >{ };

			for ( auto column = std::size_t{ 0 }; column <= current.columns( ); ++column ) {
				const auto changed = column < current.columns( ) &&
					previous.at( row, column ) != current.at( row, column );

				if ( changed && !run_start ) {
					run_start = column;

					continue;
				}

				if ( !changed && run_start ) {
					auto entry = cell_run{ };
					entry.row = row;
					entry.column = *run_start;
					entry.count = column - *run_start;

					out.push_back( entry );
					run_start.reset( );
				}
			}
		}

		return out;
	}

	ansi_emitter::ansi_emitter( const capabilities& caps ) : caps_( caps ) { }

	auto ansi_emitter::sgr( const style& value ) -> std::string {
		if ( pen_valid_ && pen_ == value ) {
			return { };
		}

		// "\x1b[0" and not "\x1b[0m": the trailing `m` closes the sequence, and
		// every parameter appended below must land INSIDE the bracket. With the
		// `m` already present the parameters were emitted after a complete
		// sequence, so the terminal executed the reset and then printed the
		// text ";90;37m" on screen.
		auto out = std::string{ "\x1b[0" };

		if ( value.bold ) {
			out += ";1";
		}

		if ( value.dim ) {
			out += ";2";
		}

		if ( value.italic ) {
			out += ";3";
		}

		if ( value.reverse ) {
			out += ";7";
		}

		if ( caps_.depth != capabilities::color_depth::none ) {
			const auto foreground = token_color( value.foreground, caps_.depth );

			if ( !foreground.empty( ) ) {
				out += ';';
				out += foreground;
			}

			const auto background = token_color( value.background, caps_.depth );

			if ( !background.empty( ) ) {
				out += ';';
				out += background;
			}
		}

		out += 'm';

		pen_ = value;
		pen_valid_ = true;

		return out;
	}

	auto ansi_emitter::move_to( const std::size_t from_row, const std::size_t to_row,
		const std::size_t column ) -> std::string {
		auto out = std::string{ };

		if ( to_row < from_row ) {
			out += "\x1b[";
			out += std::to_string( from_row - to_row );
			out += 'A';
		} else if ( to_row > from_row ) {
			out += "\x1b[";
			out += std::to_string( to_row - from_row );
			out += 'B';
		}

		out += "\x1b[";
		out += std::to_string( column + 1 );
		out += 'G';

		return out;
	}

	auto ansi_emitter::emit( const cell_buffer& previous, const cell_buffer& current )
		-> std::string {
		const auto runs = diff_rows( previous, current );

		if ( runs.empty( ) ) {
			return { };
		}

		const auto rows = current.rows( );
		const auto parked = rows == 0 ? std::size_t{ 0 } : rows - 1;

		auto out = std::string{ };
		auto here = parked;

		for ( const auto& run : runs ) {
			out += move_to( here, run.row, run.column );
			here = run.row;

			for ( auto index = std::size_t{ 0 }; index < run.count; ++index ) {
				const auto& cell_value = current.at( run.row, run.column + index );

				// The continuation slot of a wide cluster: the previous cell
				// already drew both columns.
				if ( cell_value.width == 0 ) {
					continue;
				}

				out += sgr( cell_value.cell_style );

				// A blank cell has no text, and appending nothing leaves the
				// previous frame's glyph on screen. Every erase -- a backspace,
				// a line that got shorter, the spinner finishing -- would then
				// ghost. One space repaints the column, so the diff is honest.
				if ( cell_value.text.empty( ) ) {
					out += ' ';
				} else {
					out += cell_value.text;
				}
			}
		}

		// Back to the parked row, so the next move starts from a known place.
		out += move_to( here, parked, 0 );

		pen_valid_ = false;

		return out;
	}

	auto ansi_emitter::caret( const std::size_t parked_row, const std::size_t row,
		const std::size_t column ) const -> std::string {
		return move_to( parked_row, row, column );
	}

	auto ansi_emitter::region_top( const std::size_t row_count ) const -> std::string {
		if ( row_count <= 1 ) {
			return std::string{ "\x1b[1G" };
		}

		auto out = std::string{ "\x1b[" };
		out += std::to_string( row_count - 1 );
		out += "A\x1b[1G";

		return out;
	}

	auto ansi_emitter::clear_region( const std::size_t row_count ) const -> std::string {
		auto out = region_top( row_count );

		for ( auto index = std::size_t{ 0 }; index < row_count; ++index ) {
			// EL erases the whole line without moving the cursor, which is what
			// makes this safe to repeat per row.
			out += "\x1b[2K";

			if ( index + 1 < row_count ) {
				out += "\x1b[1B";
			}
		}

		out += "\x1b[1G";

		return out;
	}

	auto ansi_emitter::synchronized( const std::string& frame ) const -> std::string {
		if ( !caps_.synchronized_output || frame.empty( ) ) {
			return frame;
		}

		auto out = std::string{ "\x1b[?2026h" };
		out += frame;
		out += "\x1b[?2026l";

		return out;
	}

}
