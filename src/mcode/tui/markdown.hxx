#pragma once

#include <string_view>

#include "mcode/tui/cell.hxx"

namespace mcode::tui {

	// Inline formatting for one line of streamed text: bold, italic and inline
	// code. One pass; emphasis does not nest.
	[[nodiscard]] auto render_inline( std::string_view text, token base ) -> styled_line;

}
