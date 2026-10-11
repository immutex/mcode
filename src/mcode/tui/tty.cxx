#include "mcode/tui/tty.hxx"

#include <cstdlib>

#include "mcode/platform/seams.hxx"
#include "mcode/support/time.hxx"

namespace mcode::tui {

	auto monotonic_ms( ) -> std::uint64_t {
		return support::monotonic_milliseconds( );
	}

	auto probe_capabilities( const std::string_view colorterm, const std::string_view term,
		const bool no_color, const bool has_tty,
		const std::string_view ambiguous_width_env ) -> capabilities {
		auto out = capabilities{ };
		out.is_tty = has_tty;

		out.ambiguous_width = ambiguous_width_env == "2" ? 2 : 1;

		// NO_COLOR wins: attributes only, no foreground or background changes.
		if ( no_color ) {
			out.depth = capabilities::color_depth::none;
		} else if ( colorterm.find( "truecolor" ) != std::string_view::npos ||
			colorterm.find( "24bit" ) != std::string_view::npos ) {
			out.depth = capabilities::color_depth::truecolor;
		} else if ( term.find( "256color" ) != std::string_view::npos ||
			colorterm.find( "256color" ) != std::string_view::npos ) {
			out.depth = capabilities::color_depth::ansi256;
		} else if ( term.find( "xterm" ) != std::string_view::npos ||
			term.find( "screen" ) != std::string_view::npos ||
			term.find( "tmux" ) != std::string_view::npos ||
			term.find( "vt100" ) != std::string_view::npos ) {
			out.depth = capabilities::color_depth::ansi256;
		} else if ( has_tty ) {
			out.depth = capabilities::color_depth::ansi16;
		} else {
			out.depth = capabilities::color_depth::none;
		}

		out.synchronized_output = has_tty;
		out.bracketed_paste = has_tty;
		out.kitty_keyboard = has_tty;

		return out;
	}

	[[nodiscard]] auto tty_environment( const char* name ) -> std::string_view {
		const auto* value = std::getenv( name );

		return value != nullptr ? std::string_view{ value } : std::string_view{ };
	}

}
