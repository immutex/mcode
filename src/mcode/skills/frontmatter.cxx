#include "mcode/skills/frontmatter.hxx"

#include <algorithm>

namespace mcode::skills {

	namespace {

		inline constexpr char DELIMITER[] = "---";

		// A hyphen is legal inside a scalar, so only the bracketed and quoted forms are here.
		inline constexpr char NESTED_KEYS[] = "[{&*!|>%@`";

		auto is_blank( const std::string_view line ) noexcept -> bool {
			return std::all_of( line.begin( ), line.end( ), []( const char character ) {
				return character == ' ' || character == '\t' || character == '\r';
			} );
		}

		auto indent_width( const std::string_view line ) -> std::size_t {
			auto width = std::size_t{ 0 };

			while ( width < line.size( ) && line[ width ] == ' ' ) {
				++width;
			}

			return width;
		}

		auto trim( const std::string_view input ) -> std::string_view {
			auto begin = std::size_t{ 0 };
			auto end = input.size( );

			while ( begin < end && ( input[ begin ] == ' ' || input[ begin ] == '\t' ) ) {
				++begin;
			}

			while ( end > begin && ( input[ end - 1 ] == ' ' || input[ end - 1 ] == '\t'
				|| input[ end - 1 ] == '\r' ) ) {
				--end;
			}

			return input.substr( begin, end - begin );
		}

		// Escapes are not expanded: the value is data the model reads, not a format we interpret.
		auto unquote( const std::string_view text ) -> std::string {
			if ( text.size( ) >= 2 && text.front( ) == '"' && text.back( ) == '"' ) {
				return std::string{ text.substr( 1, text.size( ) - 2 ) };
			}

			return std::string{ text };
		}

		// A quote must close, or the value is refused rather than guessed at.
		auto scalar_value( const std::string_view raw, std::string& out ) -> bool {
			const auto value = trim( raw );

			if ( value.empty( ) ) {
				return false;
			}

			if ( value.find_first_of( NESTED_KEYS ) != std::string_view::npos
				|| value.find( ": " ) != std::string_view::npos
				|| value.ends_with( ":" ) ) {
				return false;
			}

			if ( value.front( ) == '"' ) {
				if ( value.size( ) < 2 || value.back( ) != '"' ) {
					return false;
				}
			}

			out = unquote( value );

			return true;
		}

	} // namespace

	auto frontmatter_span( const std::string_view text ) -> result< std::size_t > {
		auto cursor = std::size_t{ 0 };

		// A BOM is skipped, not treated as part of the delimiter.
		if ( text.starts_with( "\xEF\xBB\xBF" ) ) {
			cursor = 3;
		}

		while ( cursor < text.size( ) && ( text[ cursor ] == '\r' || text[ cursor ] == '\n' ) ) {
			++cursor;
		}

		if ( !text.substr( cursor ).starts_with( DELIMITER ) ) {
			return std::unexpected( fail( errc::config, "no frontmatter block" ) );
		}

		auto line_end = text.find( '\n', cursor );

		if ( line_end == std::string_view::npos ) {
			return std::unexpected( fail( errc::config, "frontmatter is never closed" ) );
		}

		const auto open = trim( text.substr( cursor, line_end - cursor ) );

		if ( open != DELIMITER ) {
			return std::unexpected( fail( errc::config, "no frontmatter block" ) );
		}

		while ( true ) {
			const auto next = text.find( '\n', line_end + 1 );

			if ( next == std::string_view::npos ) {
				return std::unexpected( fail( errc::config, "frontmatter is never closed" ) );
			}

			if ( trim( text.substr( line_end + 1, next - line_end - 1 ) ) == DELIMITER ) {
				return next + 1;
			}

			line_end = next;
		}
	}

	auto parse_frontmatter( const std::string_view text ) -> result< frontmatter > {
		const auto span = frontmatter_span( text );

		if ( !span ) {
			return std::unexpected( span.error( ) );
		}

		auto block = text.substr( 0, *span );
		auto cursor = block.find( '\n' ) + 1;

		// Stop before the closing delimiter: a `---` line has no colon and would parse as an error.
		const auto content_end = block.size( ) > 5
			? block.rfind( '\n', block.size( ) - 5 ) : 0;

		auto parsed = frontmatter{ };
		auto saw_name = false;
		auto saw_description = false;

		while ( cursor < content_end ) {
			auto line_end = block.find( '\n', cursor );

			if ( line_end == std::string_view::npos || line_end > content_end ) {
				line_end = content_end;
			}

			const auto line = block.substr( cursor, line_end - cursor );
			cursor = line_end + 1;

			if ( is_blank( line ) ) {
				continue;
			}

			const auto indent = indent_width( line );

			if ( indent > 0 ) {
				return std::unexpected( fail( errc::config,
					"frontmatter contains an indented line; nested structures are not "
					"supported" ) );
			}

			const auto separator = line.find( ':' );

			if ( separator == std::string_view::npos || separator == 0 ) {
				return std::unexpected( fail( errc::config,
					"frontmatter line is not a key-value pair" ) );
			}

			const auto key = trim( line.substr( 0, separator ) );
			const auto raw = line.substr( separator + 1 );

			auto value = std::string{ };

			if ( key == "name" ) {
				if ( !scalar_value( raw, value ) ) {
					return std::unexpected( fail( errc::config,
						"'name' is not a plain scalar" ) );
				}

				parsed.name = std::move( value );
				saw_name = true;

				continue;
			}

			if ( key == "description" ) {
				if ( !scalar_value( raw, value ) ) {
					return std::unexpected( fail( errc::config,
						"'description' is not a plain scalar" ) );
				}

				parsed.description = std::move( value );
				saw_description = true;

				continue;
			}

			if ( key == "disable-model-invocation" ) {
				const auto flag = trim( raw );

				if ( flag == "true" ) {
					parsed.disable_model_invocation = true;

					continue;
				}

				if ( flag == "false" || flag.empty( ) ) {
					continue;
				}

				return std::unexpected( fail( errc::config,
					"'disable-model-invocation' must be true or false" ) );
			}

			// An unused key must still be a scalar: a structure there is a mistake or a mis-parse.
			if ( !scalar_value( raw, value ) ) {
				return std::unexpected( fail( errc::config,
					"'" + std::string{ key } + "' is not a plain scalar" ) );
			}
		}

		if ( !saw_name ) {
			return std::unexpected( fail( errc::config, "frontmatter has no 'name'" ) );
		}

		if ( !saw_description ) {
			return std::unexpected( fail( errc::config, "frontmatter has no 'description'" ) );
		}

		return parsed;
	}

}
