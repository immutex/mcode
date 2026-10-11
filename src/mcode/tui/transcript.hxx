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

	inline constexpr std::size_t THOUGHT_COMMIT_MAX_ROWS = 8;

	namespace transcript {

		[[nodiscard]] auto render_block( std::string_view text, token base )
			-> std::vector< styled_line >;

		[[nodiscard]] auto tail_rows( std::vector< styled_line > rows, std::size_t max_rows )
			-> std::vector< styled_line >;

		[[nodiscard]] auto prefix_row( const styled_line& row, std::string_view gutter,
			token color ) -> styled_line;

		// splits one row to the budget, keeping its indent; fenced code is returned untouched
		[[nodiscard]] auto wrap_row( const styled_line& row, std::size_t columns,
			std::size_t ambiguous_width ) -> std::vector< styled_line >;

		[[nodiscard]] auto wrap_rows( std::vector< styled_line > rows, std::size_t columns,
			std::size_t ambiguous_width ) -> std::vector< styled_line >;

	}

}
