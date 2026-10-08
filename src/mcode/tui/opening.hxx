#pragma once

#include <string>
#include <string_view>

#include "mcode/tui/render.hxx"

namespace mcode::tui {

	// What the opening banner says. A value rather than a read off the loop, so
	// the sequence that paints it is testable without building an agent loop.
	struct session_opening {
		std::string_view version;
		std::string_view platform;
		std::string_view compiler;

		// A permissive session (`approval_mode == "never"`) states its boundary,
		// because a permissive default is only defensible while what still holds
		// is visible.
		bool permissive = false;
	};

	// Queues the banner and returns the bytes that put it in scrollback with the
	// live region below it. The caller writes them; the return is what a test
	// asserts on, since the layout is a property of the byte stream and not of
	// the coordinator's state.
	[[nodiscard]] auto emit_opening( render_coordinator& coordinator,
		const session_opening& value ) -> std::string;

}
