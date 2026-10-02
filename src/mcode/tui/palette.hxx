#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcode::tui {

	// One command the palette can offer.
	struct slash_command {
		std::string name;
		std::string description;
	};

	// The palette's state: whether it is up, what filters it, the filtered
	// list, and which entry is selected.
	struct slash_palette {
		bool open = false;
		std::string query;
		std::vector< slash_command > matches;
		std::size_t selected = 0;

		[[nodiscard]] auto empty( ) const noexcept -> bool { return matches.empty( ); }
	};

	// Filters `commands` against the text after the leading '/'. An empty
	// query matches everything, so a bare '/' lists the whole set.
	[[nodiscard]] auto filter_commands( const std::vector< slash_command >& commands,
		std::string_view query ) -> std::vector< slash_command >;

	// The query in a prompt, or nullopt when the prompt is not a command.
	// A command is a leading '/' followed by a name with no space yet, so a
	// path like /tmp/notes is not mistaken for one.
	[[nodiscard]] auto command_query( std::string_view input ) -> std::optional< std::string_view >;

	// The completed line for `name`, with a trailing space so arguments can
	// follow.
	[[nodiscard]] auto completed_command( std::string_view name ) -> std::string;

}
