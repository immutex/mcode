#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace mcode::mcp {

	// Per-request timeouts. A list is cheap and a call may be slow work, but the
	// absolute maximum is enforced inside `client::call` on every request
	// regardless: a server that never answers must never block the loop forever.
	inline constexpr std::chrono::milliseconds DEFAULT_LIST_TIMEOUT{ 30'000 };
	inline constexpr std::chrono::milliseconds DEFAULT_CALL_TIMEOUT{ 120'000 };
	inline constexpr std::chrono::milliseconds ABSOLUTE_MAX_TIMEOUT{ 300'000 };

	// Restart backoff, one attempt per step, then give up and surface it.
	inline constexpr std::size_t RESTART_BACKOFF_STEPS = 5;
	inline constexpr std::chrono::milliseconds RESTART_BACKOFF_BASE{ 1'000 };

	// The protocol version this client speaks. The 2025-06-18 core is the
	// interop target; the fleet overwhelmingly speaks 2024-11-05..2025-06-18.
	inline constexpr std::string_view PROTOCOL_VERSION = "2025-06-18";

	// The client name and version reported in `initialize`.
	inline constexpr std::string_view CLIENT_NAME = "mcode";
	inline constexpr std::string_view CLIENT_VERSION = "0.0.1";

	// Rough pre-send estimate for budgeting; the same chars/4 heuristic the loop
	// uses for its own budget math.
	inline constexpr std::size_t CHARS_PER_TOKEN_ESTIMATE = 4;

	// Wraps untrusted text in delimiters before it reaches the model. A tool
	// description and a tool result are attacker-controlled input; interpolated
	// raw they are executable influence. The delimiters are this layer's own
	// convention -- there is no pre-existing one to reuse.
	inline constexpr std::string_view UNTRUSTED_BEGIN = "<mcode:untrusted>";
	inline constexpr std::string_view UNTRUSTED_END = "</mcode:untrusted>";

	[[nodiscard]] auto wrap_untrusted( std::string_view text ) -> std::string;

}
