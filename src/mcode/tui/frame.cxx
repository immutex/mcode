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

		auto out = std::string{ "\x1b[0m" };

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

	auto ansi_emitter::emit( const cell_buffer& previous, const cell_buffer& current,
		const std::size_t region_row ) -> std::string {
		const auto runs = diff_rows( previous, current );

		if ( runs.empty( ) ) {
			return { };
		}

		pen_valid_ = false;

		auto out = std::string{ };
		auto current_row = std::optional< std::size_t >{ };

		for ( const auto& run : runs ) {
			if ( !current_row || *current_row != run.row ) {
				current_row = run.row;

				out += "\x1b[";
				out += std::to_string( region_row + run.row + 1 );
				out += ";1H";
			}

			for ( auto index = std::size_t{ 0 }; index < run.count; ++index ) {
				const auto& cell_value = current.at( run.row, run.column + index );

				if ( cell_value.width == 0 ) {
					continue;
				}

				out += sgr( cell_value.cell_style );
				out += cell_value.text;
			}
		}

		pen_valid_ = false;

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

	auto ansi_emitter::clear_region( const std::size_t region_row,
		const std::size_t row_count ) const -> std::string {
		auto out = std::string{ };

		for ( auto index = std::size_t{ 0 }; index < row_count; ++index ) {
			out += "\x1b[";
			out += std::to_string( region_row + index + 1 );
			out += ";1H\x1b[2K";
		}

		return out;
	}

}
