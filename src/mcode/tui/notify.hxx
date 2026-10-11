#pragma once

#include <string>
#include <string_view>

namespace mcode::tui {

	struct notification_context {
		bool stdout_is_terminal = false;
		bool no_color = false;

		// A `TERM=dumb` terminal gets the bell instead of a sequence it would
		// print literally.
		bool osc9_supported = true;
	};

	// An OSC 9 sequence, a bare BEL, or nothing when suppressed. Control bytes
	// in `title` and `body` are replaced, so neither ends the sequence early.
	[[nodiscard]] auto notification_bytes( std::string_view title, std::string_view body,
		const notification_context& context ) -> std::string;

	// On stderr, never stdout: stdout is the `exec --json` contract.
	auto notify_terminal( std::string_view title, std::string_view body ) -> void;

}
