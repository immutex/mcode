#pragma once

#include <string>
#include <string_view>

namespace mcode::tui {

	// What the current process knows about its terminal, passed in rather than
	// read, so the decision below stays a pure function a test can drive.
	struct notification_context {
		// stdout is a terminal. A redirect means no notification at all.
		bool stdout_is_terminal = false;

		// NO_COLOR is set and non-empty: the user asked for no escapes.
		bool no_color = false;

		// The terminal has an OSC handler. A `TERM=dumb` one does not, so it
		// gets the bell instead of a sequence it would print literally.
		bool osc9_supported = true;
	};

	// The exact bytes for one notification: an OSC 9 sequence, a bare BEL, or
	// nothing when the notification is suppressed. Control bytes in `title`
	// and `body` are replaced, so neither can end the sequence early.
	[[nodiscard]] auto notification_bytes( std::string_view title, std::string_view body,
		const notification_context& context ) -> std::string;

	// Emits one notification on stderr, never stdout: stdout is the
	// `exec --json` contract, and a stray escape there would corrupt it.
	auto notify_terminal( std::string_view title, std::string_view body ) -> void;

}
