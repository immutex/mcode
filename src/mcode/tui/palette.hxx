#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcode::tui {

	struct slash_command {
		std::string name;
		std::string description;
	};

	struct slash_palette {
		bool open = false;
		std::string query;
		std::vector< slash_command > matches;
		std::size_t selected = 0;

		// A mention row's name is already a workspace-relative path, so that
		// picker clears the slash.
		std::string prefix = "/";

		[[nodiscard]] auto empty( ) const noexcept -> bool { return matches.empty( ); }

		[[nodiscard]] auto highlighted( ) const noexcept -> const slash_command* {
			return open && selected < matches.size( ) ? &matches[ selected ] : nullptr;
		}
	};

	[[nodiscard]] auto filter_commands( const std::vector< slash_command >& commands,
		std::string_view query ) -> std::vector< slash_command >;

	// A path like /tmp/notes is not a command.
	[[nodiscard]] auto command_query( std::string_view input ) -> std::optional< std::string_view >;

	[[nodiscard]] auto completed_command( std::string_view name ) -> std::string;

	// The mention picker never submits; it inserts into the editor.
	[[nodiscard]] auto submitted_line( const slash_palette& palette,
		std::string_view typed ) -> std::string;

}
