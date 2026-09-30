#include "mcode/tui/theme.hxx"

#include <array>

#include "mcode/tui/frame.hxx"

namespace mcode::tui {

	auto theme_table( ) -> const std::vector< theme_entry >& {
		static const std::vector< theme_entry > TABLE = {
			{ token::text, "#d4d4d4", "253", "default" },
			{ token::muted, "#6b6b6b", "243", "bright-black" },
			{ token::accent, "#7aa2f7", "111", "bright-blue" },
			{ token::success, "#9ece6a", "114", "green" },
			{ token::warn, "#e0af68", "179", "yellow" },
			{ token::error, "#f7768e", "204", "red" },
			{ token::code_bg, "#1a1b26", "234", "black" },
			{ token::diff_add_bg, "#20303b", "236", "" },
			{ token::diff_add_emph, "#2d4f67", "239", "" },
			{ token::diff_del_bg, "#312734", "237", "" },
			{ token::diff_del_emph, "#4a3247", "240", "" },
		};

		return TABLE;
	}

	auto resolve_token( const token value, const capabilities::color_depth depth )
		-> std::string_view {
		return token_color( value, depth );
	}

}
