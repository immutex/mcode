#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/model/types.hxx"

namespace mcode::model {

	struct auth_spec {
		enum class source {
			none,
			environment,
			config,
		};

		source from = source::none;
		std::string name;
		std::string header;
		std::string scheme = "Bearer";
	};

	struct stream_spec {
		// JSON pointers into one SSE event's data object.

		// empty means this provider never streams text.
		std::string text_delta;

		std::string thinking_delta;

		// no `/-` or `*`: the applier resolves pointers literally.
		std::string tool_call_index;
		std::string tool_call_id;
		std::string tool_call_name;
		std::string tool_call_args;

		std::string finish_reason;

		// a mid-stream failure arrives as an ordinary event, not an HTTP status.
		std::string error_message;

		std::string error_code;

		std::string usage_input;
		std::string usage_output;
		std::string usage_cached_read;
		std::string usage_cache_write;
		std::string usage_reasoning;

		// SSE `event:` names meaning the stream is over; matched against the event name.
		std::vector< std::string > terminal_events;

		// empty means every event; a gate is needed when two different things share one pointer.
		std::vector< std::string > text_events;
		std::vector< std::string > tool_call_events;
	};

	struct request_spec {
		std::string model = "model";
		std::string messages = "messages";
		std::string tools = "tools";
		std::string max_output_tokens = "max_tokens";
		std::string temperature = "temperature";
		std::string response_schema = "response_format";

		// member names inside one entry of the tools array
		std::string tool_choice = "tool_choice";
		std::string parallel_tool_calls = "parallel_tool_calls";

		// the member of a function object that switches on schema-constrained generation
		std::string tool_strict = "strict";

		std::string role_system = "system";
		std::string role_user = "user";
		std::string role_assistant = "assistant";
		std::string role_tool = "tool";
	};

	// What a gateway actually implements, as distinct from what its wire format could express.
	// Every flag defaults to the conservative answer so an older descriptor stays valid, and a
	// technique is attempted only when its flag is set.
	struct provider_features {
		// `strict: true` inside a function object, plus the strict schema shape. Required in
		// addition to the model capability: the gateway must accept the field and the model
		// must honour the grammar.
		bool strict_tools = false;

		// `parallel_tool_calls` as a request member.
		bool parallel_tool_calls = false;

		// An assistant turn whose content the model continues, sent as the last message. It
		// suppresses the prose preamble some models emit before a tool call, because the
		// answer begins mid-turn. Empty means no prefill, which is the default: the text is
		// the gateway's convention, not this harness's, so it is declared, never guessed.
		std::string prefill_text;

		// The gateway keeps calling tools even when a JSON response format is also present.
		// Most do not, so the format is dropped when a tool list is sent unless this is set.
		bool response_format_with_tools = false;
	};

	struct provider_descriptor {
		std::string name;

		std::string endpoint;

		auth_spec auth;
		request_spec request;
		stream_spec stream;

		std::string extra_headers_json;

		// when set, every SSE event goes to a host callback, not the pointer mapping.
		bool escape_hatch = false;

		// The wire capabilities this gateway actually implements. Each defaults to the
		// conservative answer, so a descriptor written before a field existed keeps working.
		// A technique is only attempted when its flag is set, and a 400 that names the field
		// downgrades the flag for the rest of the session rather than failing the run.
		provider_features features = { };
	};

	// rejects what would fail silently at first token: bad endpoint, pointer, or credential source.
	[[nodiscard]] auto validate( const provider_descriptor& descriptor ) -> status;

	// unknown keys are rejected: a typo that silently disables a field is worse than a load error.
	[[nodiscard]] auto descriptor_from_json( std::string_view json_text )
		-> result< provider_descriptor >;

	class provider_registry {
	public:
		// a duplicate name is refused rather than shadowed, which would make --model ambiguous.
		auto add( provider_descriptor descriptor, std::string owner = { } ) -> status;

		[[nodiscard]] auto find( std::string_view name ) const -> const provider_descriptor*;

		[[nodiscard]] auto all( ) const -> std::vector< const provider_descriptor* >;

		[[nodiscard]] auto owned_by( std::string_view owner ) const
			-> std::vector< const provider_descriptor* >;

		auto remove_owner( std::string_view owner ) -> std::size_t;

		[[nodiscard]] auto size( ) const noexcept -> std::size_t { return entries_.size( ); }
		[[nodiscard]] auto empty( ) const noexcept -> bool { return entries_.empty( ); }

	private:
		struct entry {
			provider_descriptor descriptor;
			std::string owner;
		};

		std::map< std::string, entry, std::less<> > entries_;
	};

}
