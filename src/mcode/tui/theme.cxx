#include "mcode/tui/theme.hxx"

#include <array>
#include <string>

namespace mcode::tui {

	namespace {

		// Size inferred, so adding a token cannot silently truncate the table.
		inline constexpr auto ENTRIES = std::to_array< theme_entry >( {
			{ token::none, "#d4d4d4", "37", "37" },
			{ token::text, "#d4d4d4", "38;5;253", "37" },
			{ token::muted, "#6b6b6b", "38;5;243", "90" },
			{ token::accent, "#7aa2f7", "38;5;111", "94" },
			{ token::success, "#9ece6a", "38;5;114", "32" },
			{ token::warn, "#e0af68", "38;5;179", "33" },
			{ token::error, "#f7768e", "38;5;204", "31" },
			{ token::thinking, "#7d7fa8", "38;5;103", "35" },
			{ token::code_bg, "#1a1b26", "48;5;234", "" },
			{ token::diff_add_bg, "#20303b", "48;5;236", "" },
			{ token::diff_add_emph, "#2d4f67", "48;5;239", "" },
			{ token::diff_del_bg, "#312734", "48;5;237", "" },
			{ token::diff_del_emph, "#4a3247", "48;5;240", "" },
		} );

		// Every token needs an entry: a missing one falls back to index 0 and
		// silently renders as the wrong colour.
		static_assert( ENTRIES.size( ) == TOKEN_COUNT );

		[[nodiscard]] constexpr auto hex_digit( const char value ) -> unsigned {
			if ( value >= '0' && value <= '9' ) {
				return static_cast< unsigned >( value - '0' );
			}

			return static_cast< unsigned >( value - 'a' ) + 10u;
		}

		// "#rrggbb" -> "38;2;r;g;b". Derived from the table, so a colour is
		// written down exactly once.
		[[nodiscard]] auto truecolor_sgr( const char* hex ) -> std::string {
			const auto channel = [ & ]( const std::size_t offset ) {
				return ( hex_digit( hex[ offset ] ) << 4 ) | hex_digit( hex[ offset + 1 ] );
			};

			return std::string{ "38;2;" } + std::to_string( channel( 1 ) ) + ";" +
				std::to_string( channel( 3 ) ) + ";" + std::to_string( channel( 5 ) );
		}

		[[nodiscard]] auto truecolor_table( ) -> const std::array< std::string, ENTRIES.size( ) >& {
			static const auto built = [ ] {
				auto out = std::array< std::string, ENTRIES.size( ) >{ };

				for ( auto index = std::size_t{ 0 }; index < ENTRIES.size( ); ++index ) {
					out[ index ] = truecolor_sgr( ENTRIES[ index ].truecolor );
				}

				return out;
			}( );

			return built;
		}

		[[nodiscard]] auto entry_index( const token value ) -> std::size_t {
			for ( auto index = std::size_t{ 0 }; index < ENTRIES.size( ); ++index ) {
				if ( ENTRIES[ index ].name == value ) {
					return index;
				}
			}

			return 0;
		}

	}

	auto theme_table( ) -> const std::vector< theme_entry >& {
		static const auto table = std::vector< theme_entry >{ ENTRIES.begin( ), ENTRIES.end( ) };

		return table;
	}

	auto token_color( const token value, const capabilities::color_depth depth ) -> std::string_view {
		// `none` is the absent sentinel, not a colour: it is the default
		// background of every span. Resolving it to a value would emit a
		// second foreground after the real one, and the terminal applies the
		// last, so every span would render in the sentinel's colour.
		if ( value == token::none || depth == capabilities::color_depth::none ) {
			return { };
		}

		const auto index = entry_index( value );

		switch ( depth ) {
			case capabilities::color_depth::truecolor:
				return truecolor_table( )[ index ];
			case capabilities::color_depth::ansi256:
				return ENTRIES[ index ].ansi256;
			case capabilities::color_depth::ansi16:
				return ENTRIES[ index ].ansi16;
			case capabilities::color_depth::none:
				return { };
		}

		return { };
	}

}
