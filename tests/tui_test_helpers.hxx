#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "mcode/tui/cell.hxx"

namespace tui_test {

	using mcode::tui::styled_line;

	inline constexpr std::size_t ROWS = 4;
	inline constexpr std::size_t COLUMNS = 40;

	[[nodiscard]] inline auto row_text( const styled_line& row ) -> std::string {
		auto text = std::string{ };

		for ( const auto& span : row ) {
			text += span.text;
		}

		return text;
	}

	// the text a frame writes, with the escape sequences removed
	[[nodiscard]] inline auto visible_text( const std::string& bytes ) -> std::string {
		auto out = std::string{ };

		for ( auto index = std::size_t{ 0 }; index < bytes.size( ); ++index ) {
			if ( bytes[ index ] != '\x1b' ) {
				out.push_back( bytes[ index ] );

				continue;
			}

			++index;

			while ( index < bytes.size( ) &&
				!std::isalpha( static_cast< unsigned char >( bytes[ index ] ) ) ) {
				++index;
			}
		}

		return out;
	}

	// A state's rendered rows against a fresh render of the same buffer: a
	// cache that lags shows up as a text difference, which is what this
	// compares.
	[[nodiscard]] inline auto same_rows( const std::vector< styled_line >& left,
		const std::vector< styled_line >& right ) -> bool {
		if ( left.size( ) != right.size( ) ) {
			return false;
		}

		for ( auto index = std::size_t{ 0 }; index < left.size( ); ++index ) {
			if ( row_text( left[ index ] ) != row_text( right[ index ] ) ) {
				return false;
			}
		}

		return true;
	}

} // namespace tui_test
