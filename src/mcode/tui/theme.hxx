#pragma once

#include "mcode/tui/cell.hxx"
#include "mcode/tui/tty.hxx"
#include <string_view>
#include <vector>

namespace mcode::tui {

	// The theme table. Each token declares its {truecolor, ansi256, ansi16}
	// triple; the renderer resolves the whole table once per session at the
	// probed depth, never per cell. With no colour at all the resolution is
	// attributes only: bold for emphasis, no fg or bg changes.
	//
	// The token names and the fallback rule are the durable part; the values
	// are chosen against a real terminal and live in frame.cxx's
	// token_color, which is the one place a depth is consulted.
	struct theme_entry {
		token name;
		const char* truecolor;
		const char* ansi256;
		const char* ansi16;
	};

	// The table itself, for documentation and tests. The emitter reads
	// token_color; this is the declared contract the values must match.
	[[nodiscard]] auto theme_table( ) -> const std::vector< theme_entry >&;

	// Resolves one token at one depth. The single resolution point.
	[[nodiscard]] auto resolve_token( token value, capabilities::color_depth depth )
		-> std::string_view;

}
