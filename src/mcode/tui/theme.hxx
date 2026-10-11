#pragma once

#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::tui {

	struct theme_entry {
		token name;
		const char* truecolor;
		const char* ansi256;
		const char* ansi16;
	};

	[[nodiscard]] auto theme_table( ) -> const std::vector< theme_entry >&;

	// Empty means no colour, and attributes carry the emphasis.
	[[nodiscard]] auto token_color( token value, capabilities::color_depth depth )
		-> std::string_view;

	// Through `token_color` it would emit a second foreground that overrides the
	// span's real one.
	[[nodiscard]] auto token_background( token value, capabilities::color_depth depth )
		-> std::string_view;

}
