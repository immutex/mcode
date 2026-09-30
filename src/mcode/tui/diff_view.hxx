#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include <cstddef>
#include <cstdint>

namespace mcode::tui {

	// One rendered diff line.
	struct diff_line {
		enum class kind : std::uint8_t { context, added, removed };

		kind type = kind::context;
		std::string text;
		styled_line spans;
	};

	// Renders a unified diff body into tinted lines. Added lines are
	// highlighted, removed lines plain; paired -/+ lines within the threshold
	// get word-level emphasis on the changed tokens only.
	[[nodiscard]] auto render_diff( std::string_view diff_text, token base )
		-> std::vector< diff_line >;

	// Folds unchanged runs of >= FOLD_THRESHOLD context lines into one
	// `... N lines` row.
	[[nodiscard]] auto fold_context( std::vector< diff_line > lines )
		-> std::vector< diff_line >;

	// The word-level LCS over two lines' `\w+` tokens. Returns the emphasized
	// styled line for the ADDED side; the removed side stays plain.
	[[nodiscard]] auto emphasize_added( std::string_view removed_line,
		std::string_view added_line, token base ) -> styled_line;

	inline constexpr std::size_t FOLD_THRESHOLD = 3;
	inline constexpr std::size_t PAIR_THRESHOLD = 2;

}
