#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/theme.hxx"
#include "mcode/tui/tty.hxx"

// The wizard's terminal surface: the colour painter, the line and menu
// reader, and the two output helpers. Split from the flow, which does not
// need to know how a row is coloured.
namespace mcode::cli::detail {

	using tui::token;

	// A verification turn that never returns is worse than a failed one.
	inline constexpr std::uint32_t SETUP_READ_TIMEOUT_MS = 300'000;

	inline constexpr std::size_t RULE_WIDTH = 62;
	inline constexpr std::size_t MAX_DIGIT_CHOICE = 9;

	// One SGR wrapper for both the painter and the reader. They each had their
	// own copy, so a change to the escape sequence reached one and not the other.
	[[nodiscard]] inline auto paint_text( const tui::capabilities& caps, const token colour,
		const std::string_view text, const bool bold ) -> std::string {
		const auto code = tui::token_color( colour, caps.depth );

		if ( code.empty( ) ) {
			return std::string{ text };
		}

		auto out = std::string{ "\x1b[0" };

		if ( bold ) {
			out += ";1";
		}

		out += ';';
		out += code;
		out += 'm';
		out += text;
		out += "\x1b[0m";

		return out;
	}

	class painter {
	public:
		explicit painter( const tui::capabilities& caps ) : caps_( caps ) { }

		[[nodiscard]] auto paint( const token colour, const std::string_view text,
			const bool bold = false ) const -> std::string {
			return paint_text( caps_, colour, text, bold );
		}

		[[nodiscard]] auto dim( const std::string_view text ) const -> std::string {
			return paint( token::muted, text );
		}

		[[nodiscard]] auto strong( const std::string_view text ) const -> std::string {
			return paint( token::text, text, true );
		}

		[[nodiscard]] auto good( const std::string_view text ) const -> std::string {
			return paint( token::success, text );
		}

		[[nodiscard]] auto bad( const std::string_view text ) const -> std::string {
			return paint( token::error, text );
		}

		[[nodiscard]] auto mark( const std::string_view text ) const -> std::string {
			return paint( token::accent, text, true );
		}

		[[nodiscard]] auto accent( const std::string_view text ) const -> std::string {
			return paint( token::accent, text );
		}

	private:
		tui::capabilities caps_;
	};

	inline auto write_line( const std::string& text ) -> void {
		std::fputs( text.c_str( ), stdout );
		std::fputc( '\n', stdout );
		std::fflush( stdout );
	}

	inline auto blank( ) -> void {
		std::fputc( '\n', stdout );
		std::fflush( stdout );
	}

	// -------------------------------------------------------------------
	// Input. One reader, so the interactive and scripted paths behave the
	// same and a closed stdin is reported rather than read as empty.
	// -------------------------------------------------------------------

	class reader {
	public:
		explicit reader( tui::tty_session& terminal ) : terminal_( terminal ) { }

		[[nodiscard]] auto line( const std::string& prompt ) -> std::optional< std::string > {
			std::fputs( prompt.c_str( ), stdout );
			std::fflush( stdout );

			auto value = terminal_.read_line( SETUP_READ_TIMEOUT_MS );

			if ( !value ) {
				blank( );
			}

			return value;
		}

		// An arrow-key menu. The highlight starts on `preselected`, so Enter
		// takes the recommended answer, and a digit jumps straight to a row.
		[[nodiscard]] auto choose( const std::string& title,
			const std::vector< std::string >& options,
			const std::vector< std::string >& notes, const std::size_t preselected )
			-> std::optional< std::size_t > {
			auto selected = std::min( preselected, options.size( ) - 1 );

			write_line( title );
			blank( );

			const auto draw = [&]( ) -> void {
				for ( auto index = std::size_t{ 0 }; index < options.size( ); ++index ) {
					const auto active = index == selected;
					auto row = std::string{ "   " };

					row += active ? accent( ">" ) : " ";
					row += ' ';
					row += active ? strong( options[ index ] ) : options[ index ];

					if ( index < notes.size( ) && !notes[ index ].empty( ) ) {
						row += "  ";
						row += dim( notes[ index ] );
					}

					write_line( row );
				}
			};

			draw( );

			const auto drawn = options.size( );

			for ( ;; ) {
				const auto key = terminal_.read_key( SETUP_READ_TIMEOUT_MS );

				if ( key.type == tui::key_event::kind::exit
					|| key.type == tui::key_event::kind::interrupt ) {
					return std::nullopt;
				}

				if ( key.type == tui::key_event::kind::enter ) {
					return selected;
				}

				if ( key.type == tui::key_event::kind::timeout ) {
					continue;
				}

				if ( key.type == tui::key_event::kind::up ) {
					if ( selected == 0 ) {
						continue;
					}

					--selected;
				} else if ( key.type == tui::key_event::kind::down ) {
					if ( selected + 1 >= options.size( ) ) {
						continue;
					}

					++selected;
				} else if ( key.type == tui::key_event::kind::character
					&& !key.text.empty( ) ) {
					const auto digit = key.text.front( );
					const auto limit = static_cast< char >( '0' + MAX_DIGIT_CHOICE );

					if ( digit < '1' || digit > limit ) {
						continue;
					}

					const auto index = static_cast< std::size_t >( digit - '1' );

					if ( index >= options.size( ) || index == selected ) {
						continue;
					}

					selected = index;
				} else {
					continue;
				}

				// Move back over the menu and repaint it, rather than clearing
				// the screen, so the banner stays visible.
				std::fprintf( stdout, "\x1b[%zuA", drawn );
				std::fflush( stdout );

				draw( );
			}
		}

		// A secret is read the same way and simply never echoed back.
		[[nodiscard]] auto secret( const std::string& prompt )
			-> std::optional< std::string > {
			return line( prompt );
		}

	private:
		[[nodiscard]] auto styled( const token colour, const std::string_view text,
			const bool bold ) const -> std::string {
			return paint_text( terminal_.caps( ), colour, text, bold );
		}

		[[nodiscard]] auto strong( const std::string_view text ) const -> std::string {
			return styled( token::text, text, true );
		}

		[[nodiscard]] auto dim( const std::string_view text ) const -> std::string {
			return styled( token::muted, text, false );
		}

		[[nodiscard]] auto accent( const std::string_view text ) const -> std::string {
			return styled( token::accent, text, true );
		}

		tui::tty_session& terminal_;
	};

}
