// The platform-neutral half of the terminal layer: the capability probe and
// the environment helper. The console itself is per-platform, in tty_windows.cxx
// and tty_posix.cxx.
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

		// Clamped to exactly {1, 2}: an unknown value is not an error, it is
		// the one-column default, and nothing is logged.
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

		// The synchronized-output and Kitty probes write to the terminal, so
		// they are gated on a real tty. DECRQM is the gate, never TERM text.
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
