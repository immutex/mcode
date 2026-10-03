#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcode::tui {

	// One row the palette can offer. Slash commands and the mention picker
	// share it: a mention row's `name` is a workspace-relative path and its
	// `description` is empty.
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

		// Painted before every row's name. Slash commands keep the leading
		// slash; the mention picker clears it, because a row's name is already
		// a workspace-relative path.
		std::string prefix = "/";

		[[nodiscard]] auto empty( ) const noexcept -> bool { return matches.empty( ); }

		// The row the selection points at, or nullptr when none is: a closed
		// palette and a filtered-to-nothing one both have none. Enter and Tab
		// act on this row rather than on the raw typed text.
		[[nodiscard]] auto highlighted( ) const noexcept -> const slash_command* {
			return open && selected < matches.size( ) ? &matches[ selected ] : nullptr;
		}
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
	// follow. This is what Tab inserts.
	[[nodiscard]] auto completed_command( std::string_view name ) -> std::string;

	// The line Enter submits for a command palette: the highlighted row's own
	// command when one is highlighted, otherwise exactly what was typed. This
	// is what makes Enter on a `/model` row run `/model` instead of the raw
	// `/`. The mention picker never submits: it inserts into the editor.
	[[nodiscard]] auto submitted_line( const slash_palette& palette,
		std::string_view typed ) -> std::string;

}
