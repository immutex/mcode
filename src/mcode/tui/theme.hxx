#pragma once

#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::tui {

	// One token's value at each depth. `truecolor` is "#rrggbb"; the other two
	// are SGR parameters, already in their final form.
	struct theme_entry {
		token name;
		const char* truecolor;
		const char* ansi256;
		const char* ansi16;
	};

	// The declared theme, for documentation and tests.
	[[nodiscard]] auto theme_table( ) -> const std::vector< theme_entry >&;

	// Resolves one token at one depth. The single resolution point: the values
	// live in `theme_table` and nowhere else, so the table cannot drift from
	// what the emitter writes. Empty means no colour, and attributes carry the
	// emphasis.
	[[nodiscard]] auto token_color( token value, capabilities::color_depth depth )
		-> std::string_view;

	// The same resolution for a background. The table holds one value per
	// token written for the foreground, so this rewrites a "38;" sequence into
	// its "48;" form; reading it through `token_color` would emit a second
	// foreground that overrides the span's real one.
	[[nodiscard]] auto token_background( token value, capabilities::color_depth depth )
		-> std::string_view;

}
