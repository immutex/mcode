#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"

namespace mcode::tui {

	// One column wide, so every gutter lines up.
	inline constexpr std::string_view GUTTER_THOUGHT = "✻";
	inline constexpr std::string_view GUTTER_DONE = "✓";
	inline constexpr std::string_view GUTTER_OUTPUT = "│";

	// A committed thought collapses to this many rows: enough to follow the
	// reasoning, not enough to bury the answer that follows it.
	inline constexpr std::size_t THOUGHT_COMMIT_MAX_ROWS = 8;

	// The block pass the live region and the commit share. Both draw the same
	// rows: what streams is what lands in scrollback.
	namespace transcript {

		// Renders streamed text as a block, without the blank rows a run of
		// trailing newlines would leave behind.
		[[nodiscard]] auto render_block( std::string_view text, token base )
			-> std::vector< styled_line >;

		// Keeps the newest rows: a block taller than its budget shows the end
		// of the text, which is where the user is looking.
		[[nodiscard]] auto tail_rows( std::vector< styled_line > rows, std::size_t max_rows )
			-> std::vector< styled_line >;

		// Prefixes one row with a gutter, so a block reads as one group.
		[[nodiscard]] auto prefix_row( const styled_line& row, std::string_view gutter,
			token color ) -> styled_line;

		// Splits one committed row to the caller's column budget. The split
		// lands on the last space that fits, so a word is never cut in half
		// unless it alone is wider than the budget, and then it is cut on a
		// cluster boundary. Every continuation row repeats the row's own
		// leading indent, so wrapped text hangs under its content rather than
		// falling back to column 0. Fenced code is verbatim by contract and is
		// returned untouched; its only signal is the code background token.
		[[nodiscard]] auto wrap_row( const styled_line& row, std::size_t columns,
			std::size_t ambiguous_width ) -> std::vector< styled_line >;

		// Wraps every row of a block, in order, with `wrap_row`.
		[[nodiscard]] auto wrap_rows( std::vector< styled_line > rows, std::size_t columns,
			std::size_t ambiguous_width ) -> std::vector< styled_line >;

	}

}
