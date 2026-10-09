#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include "mcode/support/text.hxx"

namespace mcode::mcp {

	// enforced in client::call on every request: a silent server must not block the loop forever
	inline constexpr std::chrono::milliseconds DEFAULT_LIST_TIMEOUT{ 30'000 };
	inline constexpr std::chrono::milliseconds DEFAULT_CALL_TIMEOUT{ 120'000 };
	inline constexpr std::chrono::milliseconds ABSOLUTE_MAX_TIMEOUT{ 300'000 };

	// one attempt per step, then give up and surface it
	inline constexpr std::size_t RESTART_BACKOFF_STEPS = 5;
	inline constexpr std::chrono::milliseconds RESTART_BACKOFF_BASE{ 1'000 };

	// A `tools/list` pagination bound. A server that never stops handing back a
	// cursor would otherwise make the listing run forever, one request per page.
	inline constexpr std::size_t MAX_TOOL_PAGES = 64;

	// pinned interop target; the fleet speaks 2024-11-05..2025-06-18
	inline constexpr std::string_view PROTOCOL_VERSION = "2025-06-18";

	inline constexpr std::string_view CLIENT_NAME = "mcode";
	inline constexpr std::string_view CLIENT_VERSION = "0.0.1";

	// chars/4, matching the loop's own budget math
	inline constexpr std::size_t CHARS_PER_TOKEN_ESTIMATE = mcode::text::CHARS_PER_TOKEN;

	// tool descriptions and results are attacker-controlled input; the delimiters are ours
	inline constexpr std::string_view UNTRUSTED_BEGIN = "<mcode:untrusted>";
	inline constexpr std::string_view UNTRUSTED_END = "</mcode:untrusted>";

	[[nodiscard]] auto wrap_untrusted( std::string_view text ) -> std::string;

}
