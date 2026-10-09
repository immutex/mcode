#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/model/types.hxx"

namespace mcode::agent {

	// Conversation messages are serialised into the event log so a session can be
	// reopened with its transcript intact. Without this the log records only that
	// a tool ran, never what was asked or answered, and `--continue` restores a
	// sequence number rather than a conversation.
	//
	// The envelope is deliberately the same shape as the model's own types, so a
	// reader can be written against one definition.
	[[nodiscard]] auto message_to_json( const model::message& message ) -> std::string;

	[[nodiscard]] auto message_from_json( std::string_view json_text )
		-> result< model::message >;

	// A whole conversation, for the one event that carries the initial task.
	[[nodiscard]] auto history_to_json( const std::vector< model::message >& history )
		-> std::string;

	[[nodiscard]] auto history_from_json( std::string_view json_text )
		-> result< std::vector< model::message > >;

	// Event kinds this module owns. Named here so the writer and the reader
	// cannot disagree about a string.
	inline constexpr std::string_view MESSAGE_USER_EVENT = "message.user";
	inline constexpr std::string_view MESSAGE_ASSISTANT_EVENT = "message.assistant";
	inline constexpr std::string_view TOOL_RESULT_EVENT = "tool.output";

	// The model's reasoning for one response, recorded for analysis only.
	//
	// It is deliberately NOT a `model::block` in the assistant message: reasoning
	// is not replayed to the provider, so putting it in the history would send
	// back text the wire format does not expect. The log keeps it because a run
	// cannot be understood without it -- a repetition loop is visible in the
	// reasoning long before it shows in the answer.
	inline constexpr std::string_view MESSAGE_THINKING_EVENT = "message.thinking";

	// One provider round trip's token accounting. Recorded per request, because a
	// run total cannot show a cache that only starts hitting after the first call
	// nor a request whose prefix changed.
	inline constexpr std::string_view MODEL_USAGE_EVENT = "model.usage";

}
