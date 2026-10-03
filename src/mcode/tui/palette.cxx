#include "mcode/tui/palette.hxx"

#include <algorithm>
#include <optional>

namespace mcode::tui {

	auto filter_commands( const std::vector< slash_command >& commands,
		const std::string_view query ) -> std::vector< slash_command > {
		if ( query.empty( ) ) {
			return commands;
		}

		auto out = std::vector< slash_command >{ };

		// A prefix match first, so typing `/mo` offers `/model` ahead of a
		// command that merely contains "mo". Order within each group is the
		// registration order, which keeps the list stable as it filters.
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

		// A space ends the name: past that point the user is typing arguments,
		// not searching for a command.
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

		// The row's own name, not the typed prefix: Enter runs the command the
		// user selected. No trailing space, because this line is submitted
		// rather than edited further.
		return std::string{ "/" } + row->name;
	}

}
