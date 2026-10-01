#include "mcode/tui/frame.hxx"

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

	auto token_color( const token value, const capabilities::color_depth depth )
		-> std::string_view {
		// The {truecolor, ansi256, ansi16} triples. With no colour at all the
		// emitter drops the SGR fragment entirely and attributes carry the
		// emphasis.
		switch ( value ) {
			case token::none:
			case token::text: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "38;2;212;212;212";
					case capabilities::color_depth::ansi256: return "38;5;253";
					case capabilities::color_depth::ansi16: return "37";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
			case token::muted: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "38;2;107;107;107";
					case capabilities::color_depth::ansi256: return "38;5;243";
					case capabilities::color_depth::ansi16: return "90";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
			case token::accent: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "38;2;122;162;247";
					case capabilities::color_depth::ansi256: return "38;5;111";
					case capabilities::color_depth::ansi16: return "94";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
			case token::success: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "38;2;158;206;106";
					case capabilities::color_depth::ansi256: return "38;5;114";
					case capabilities::color_depth::ansi16: return "32";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
			case token::warn: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "38;2;224;175;104";
					case capabilities::color_depth::ansi256: return "38;5;179";
					case capabilities::color_depth::ansi16: return "33";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
			case token::error: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "38;2;247;118;142";
					case capabilities::color_depth::ansi256: return "38;5;204";
					case capabilities::color_depth::ansi16: return "31";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
			case token::code_bg: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "48;2;26;27;38";
					case capabilities::color_depth::ansi256: return "48;5;234";
					case capabilities::color_depth::ansi16: return "";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
			case token::diff_add_bg: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "48;2;32;48;59";
					case capabilities::color_depth::ansi256: return "48;5;236";
					case capabilities::color_depth::ansi16: return "";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
			case token::diff_add_emph: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "48;2;45;79;103";
					case capabilities::color_depth::ansi256: return "48;5;239";
					case capabilities::color_depth::ansi16: return "";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
			case token::diff_del_bg: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "48;2;49;39;52";
					case capabilities::color_depth::ansi256: return "48;5;237";
					case capabilities::color_depth::ansi16: return "";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
			case token::diff_del_emph: {
				switch ( depth ) {
					case capabilities::color_depth::truecolor: return "48;2;74;50;71";
					case capabilities::color_depth::ansi256: return "48;5;240";
					case capabilities::color_depth::ansi16: return "";
					case capabilities::color_depth::none: return "";
				}

				break;
			}
		}

		return "";
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

		// Movement is relative to the parked cursor, so the region sits
		// wherever the cursor is. Absolute row addressing pinned it to the top
		// of the screen and repainted over the transcript's scrollback.
		const auto move_to = [ & ]( const std::size_t row, const std::size_t column ) {
			if ( row < here ) {
				out += "\x1b[";
				out += std::to_string( here - row );
				out += 'A';
			} else if ( row > here ) {
				out += "\x1b[";
				out += std::to_string( row - here );
				out += 'B';
			}

			// CHA to the run's FIRST column. Positioning to column 1 and
			// writing the run regardless put every run that did not start at
			// the row's left edge at the wrong offset -- a status line that
			// shrank repainted its tail over its head, and the two interleaved.
			out += "\x1b[";
			out += std::to_string( column + 1 );
			out += 'G';

			here = row;
		};

		for ( const auto& run : runs ) {
			move_to( run.row, run.column );

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

		// Back to the parked row, so the next move starts from a known place
		// and the prompt is where the cursor is left.
		move_to( parked, 0 );

		pen_valid_ = false;

		return out;
	}

	auto ansi_emitter::caret( const std::size_t parked_row, const std::size_t row,
		const std::size_t column ) const -> std::string {
		auto out = std::string{ };

		if ( row < parked_row ) {
			out += "\x1b[";
			out += std::to_string( parked_row - row );
			out += 'A';
		} else if ( row > parked_row ) {
			out += "\x1b[";
			out += std::to_string( row - parked_row );
			out += 'B';
		}

		out += "\x1b[";
		out += std::to_string( column + 1 );
		out += 'G';

		return out;
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

	auto ansi_emitter::down( const std::size_t count ) const -> std::string {
		if ( count == 0 ) {
			return { };
		}

		auto out = std::string{ "\x1b[" };
		out += std::to_string( count );
		out += 'B';

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
