#pragma once

#include <string>
#include <string_view>

#include "mcode/tui/render.hxx"

namespace mcode::tui {

	struct session_opening {
		std::string_view version;
		std::string_view platform;
		std::string_view compiler;

		// A permissive session states its boundary, because a permissive
		// default is only defensible while what still holds is visible.
		bool permissive = false;
	};

	// Queues the banner and returns the bytes that put it in scrollback with
	// the live region below it; the layout is a property of that byte stream.
	[[nodiscard]] auto emit_opening( render_coordinator& coordinator,
		const session_opening& value ) -> std::string;

}
