#pragma once

#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"

namespace mcode::tui {

	// Inline formatting for one line: bold, italic and inline code. One pass;
	// emphasis does not nest.
	[[nodiscard]] auto render_inline( std::string_view text, token base ) -> styled_line;

	// One styled_line per rendered row.
	[[nodiscard]] auto render_markdown( std::string_view text, token base )
		-> std::vector< styled_line >;

}
