#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/model/types.hxx"

namespace mcode::agent {

	[[nodiscard]] auto message_to_json( const model::message& message ) -> std::string;

	[[nodiscard]] auto message_from_json( std::string_view json_text )
		-> result< model::message >;

	[[nodiscard]] auto history_to_json( const std::vector< model::message >& history )
		-> std::string;

	[[nodiscard]] auto history_from_json( std::string_view json_text )
		-> result< std::vector< model::message > >;

	inline constexpr std::string_view MESSAGE_USER_EVENT = "message.user";
	inline constexpr std::string_view MESSAGE_ASSISTANT_EVENT = "message.assistant";
	inline constexpr std::string_view TOOL_RESULT_EVENT = "tool.output";

	// Recorded for analysis only: reasoning is never replayed to the provider, so it
	// is not a `model::block` in the assistant message.
	inline constexpr std::string_view MESSAGE_THINKING_EVENT = "message.thinking";

	// Per request: a run total cannot show a cache that only starts hitting later.
	inline constexpr std::string_view MODEL_USAGE_EVENT = "model.usage";

}
