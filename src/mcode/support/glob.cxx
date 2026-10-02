#include "mcode/support/glob.hxx"

namespace mcode::support {

	auto glob_segments( const std::string_view text ) -> std::vector< std::string_view > {
		auto parts = std::vector< std::string_view >{ };
		auto start = std::size_t{ 0 };

		while ( start <= text.size( ) ) {
			const auto slash = text.find( '/', start );
			const auto end = ( slash == std::string_view::npos ) ? text.size( ) : slash;

			if ( end > start ) {
				parts.push_back( text.substr( start, end - start ) );
			}

			if ( slash == std::string_view::npos ) {
				break;
			}

			start = slash + 1;
		}

		return parts;
	}

	auto wildcard_match( const std::string_view pattern, const std::string_view text )
		-> bool {
		auto pattern_index = std::size_t{ 0 };
		auto text_index = std::size_t{ 0 };
		auto star_pattern = std::string_view::npos;
		auto star_text = std::size_t{ 0 };

		while ( text_index < text.size( ) ) {
			if ( pattern_index < pattern.size( ) &&
				( pattern[ pattern_index ] == '?' ||
					pattern[ pattern_index ] == text[ text_index ] ) ) {
				++pattern_index;
				++text_index;

				continue;
			}

			if ( pattern_index < pattern.size( ) && pattern[ pattern_index ] == '*' ) {
				star_pattern = pattern_index++;
				star_text = text_index;

				continue;
			}

			if ( star_pattern != std::string_view::npos ) {
				pattern_index = star_pattern + 1;
				text_index = ++star_text;

				continue;
			}

			return false;
		}

		while ( pattern_index < pattern.size( ) && pattern[ pattern_index ] == '*' ) {
			++pattern_index;
		}

		return pattern_index == pattern.size( );
	}

	auto glob_match( const std::string_view pattern, const std::string_view path ) -> bool {
		auto pattern_parts = glob_segments( pattern );
		auto path_parts = glob_segments( path );

		if ( pattern_parts.empty( ) ) {
			return false;
		}

		// iterative over (pattern index, path index) with `**` fan-out.
		auto states = std::vector< std::pair< std::size_t, std::size_t > >{ { 0, 0 } };

		while ( !states.empty( ) ) {
			const auto [ pattern_index, path_index ] = states.back( );
			states.pop_back( );

			if ( pattern_index == pattern_parts.size( ) &&
				path_index == path_parts.size( ) ) {
				return true;
			}

			if ( pattern_index >= pattern_parts.size( ) ) {
				continue;
			}

			const auto& part = pattern_parts[ pattern_index ];

			if ( part == "**" ) {
				for ( auto skip = path_index; skip <= path_parts.size( ); ++skip ) {
					states.emplace_back( pattern_index + 1, skip );
				}

				continue;
			}

			if ( path_index >= path_parts.size( ) ) {
				continue;
			}

			if ( wildcard_match( part, path_parts[ path_index ] ) ) {
				states.emplace_back( pattern_index + 1, path_index + 1 );
			}
		}

		return false;
	}

}
