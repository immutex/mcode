#include "mcode/tui/palette.hxx"

#include <algorithm>
#include <optional>

namespace mcode::tui {

	auto palette_window_for( const std::size_t total, const std::size_t selected,
		const std::size_t max_rows ) -> palette_window {
		if ( total == 0 || max_rows == 0 ) {
			return { };
		}

		const auto count = std::min( total, max_rows );

		// While the selection is inside the first window, the window does not move.
		auto first = selected < count ? std::size_t{ 0 } : selected - count + 1;

		// The tail is the one case the selection cannot pin: the last row of the
		// list is the last row of the window.
		if ( first + count > total ) {
			first = total - count;
		}

		return { first, count };
	}

	auto filter_commands( const std::vector< slash_command >& commands,
		const std::string_view query ) -> std::vector< slash_command > {
		if ( query.empty( ) ) {
			return commands;
		}

		auto out = std::vector< slash_command >{ };

		// Prefix matches rank first; ties keep registration order.
		for ( const auto& entry : commands ) {
			if ( entry.name.starts_with( query ) ) {
				out.push_back( entry );
			}
		}

		for ( const auto& entry : commands ) {
			if ( !entry.name.starts_with( query ) &&
				entry.name.find( query ) != std::string::npos ) {
				out.push_back( entry );
			}
		}

		return out;
	}

	auto command_query( const std::string_view input ) -> std::optional< std::string_view > {
		if ( input.empty( ) || input.front( ) != '/' ) {
			return std::nullopt;
		}

		const auto body = input.substr( 1 );

		// A space ends the name: past it the user is typing arguments.
		if ( body.find( ' ' ) != std::string_view::npos ) {
			return std::nullopt;
		}

		// A second slash means a path, not a command.
		if ( body.find( '/' ) != std::string_view::npos ) {
			return std::nullopt;
		}

		return body;
	}

	auto completed_command( const std::string_view name ) -> std::string {
		return std::string{ "/" } + std::string{ name } + " ";
	}

	auto submitted_line( const slash_palette& palette, const std::string_view typed )
		-> std::string {
		const auto* row = palette.highlighted( );

		if ( row == nullptr ) {
			return std::string{ typed };
		}

		// The row's own name, not the typed prefix; no trailing space, since
		// this line is submitted rather than edited.
		return std::string{ "/" } + row->name;
	}

}
