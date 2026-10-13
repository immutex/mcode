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

	// The palette scrolls past this many rows rather than growing. The live region
	// has a ceiling, and a list longer than the space it asks for loses its first
	// rows to the clip, because the region is painted from the bottom up.
	inline constexpr std::size_t PALETTE_MAX_ROWS = 10;

	// The visible slice of a palette's matches.
	struct palette_window {
		std::size_t first = 0;
		std::size_t count = 0;

		// The rows hidden above and below the slice, so the renderer can say so
		// without spending a line on it.
		[[nodiscard]] auto hidden_above( ) const noexcept -> bool { return first > 0; }

		[[nodiscard]] auto hidden_below( const std::size_t total ) const noexcept -> bool {
			return first + count < total;
		}
	};

	// Pinned to the top until the selection reaches the last visible row, then it
	// follows. Pinning matters: a window that centred the selection would hide the
	// first entries at rest, which is the state this exists to fix.
	[[nodiscard]] auto palette_window_for( std::size_t total, std::size_t selected,
		std::size_t max_rows ) -> palette_window;

	[[nodiscard]] auto filter_commands( const std::vector< slash_command >& commands,
		std::string_view query ) -> std::vector< slash_command >;

	// A path like /tmp/notes is not a command.
	[[nodiscard]] auto command_query( std::string_view input ) -> std::optional< std::string_view >;

	[[nodiscard]] auto completed_command( std::string_view name ) -> std::string;

	// The mention picker never submits; it inserts into the editor.
	[[nodiscard]] auto submitted_line( const slash_palette& palette,
		std::string_view typed ) -> std::string;

}
