#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/model/types.hxx"

namespace mcode::model {

	// A provider described as data (docs/26 decision 2).
	//
	// The hot path stays in C++: HTTP, SSE framing, retry, the error taxonomy,
	// and egress policy are not negotiable, and per-token delta parsing through a
	// scripting VM is the one continuously measurable cost. What Lua supplies is
	// the *mapping* -- which JSON pointer carries a text delta, which carries a
	// tool-call fragment. C++ applies it natively.
	//
	// An extension registers one of these; it never sees a token.

	struct auth_spec {
		enum class source {
			// No credential. Only valid for a local endpoint.
			none,
			// Read the environment variable named in `name`.
			environment,
			// Read the value from mcode config under `name`.
			config,
		};

		source from = source::none;
		std::string name;
		std::string header;
		std::string scheme = "Bearer";
	};

	struct stream_spec {
		// JSON pointers into one SSE event's data object.

		// Text delta. Empty means this provider never streams text.
		std::string text_delta;

		// Reasoning/thinking delta, for providers that separate it.
		std::string thinking_delta;

		// Tool calls. A pointer ending in `/-` or containing a wildcard is not
		// supported: fragments arrive per index and the applier tracks indices
		// from `tool_call_index`.
		std::string tool_call_index;
		std::string tool_call_id;
		std::string tool_call_name;
		std::string tool_call_args;

		// End of turn.
		std::string finish_reason;

		// Usage, reported once or cumulatively -- `usage::add` takes the max.
		std::string usage_input;
		std::string usage_output;
		std::string usage_cached_read;
		std::string usage_cache_write;
		std::string usage_reasoning;

		// SSE `event:` names that mean the stream is over. Checked against the
		// event name, not the data.
		std::vector< std::string > terminal_events;
	};

	struct request_spec {
		// Where the canonical fields go in the outgoing JSON body. Empty means
		// "use the provider's documented default", which for an OpenAI-compatible
		// endpoint is the field's own name.
		std::string model = "model";
		std::string messages = "messages";
		std::string tools = "tools";
		std::string max_output_tokens = "max_tokens";
		std::string temperature = "temperature";
		std::string response_schema = "response_format";

		// Rendered into every message's role field. Some gateways use different
		// words than the canonical four.
		std::string role_system = "system";
		std::string role_user = "user";
		std::string role_assistant = "assistant";
		std::string role_tool = "tool";
	};

	struct provider_descriptor {
		std::string name;

		// Full request URL. No templating: a provider that needs a computed path
		// is exotic enough to want `on_event`.
		std::string endpoint;

		auth_spec auth;
		request_spec request;
		stream_spec stream;

		// Sent as-is on every request. For gateway-specific flags.
		std::string extra_headers_json;

		// Escape hatch (D4). When set, the applier delegates every event to a
		// host-supplied callback instead of applying the pointer mapping, and the
		// stream.* pointers may all be empty.
		//
		// Opt-in and per-provider, because it moves per-token work into the VM: the
		// callback runs once per SSE event, and `28` measures what that costs
		// against the declarative path. A provider that the pointers can express
		// must not use this.
		bool escape_hatch = false;
	};

	// Validates a descriptor. Rejects the cases that would otherwise fail
	// silently at first token: a missing endpoint, a non-absolute URL, a text
	// pointer that is not a JSON pointer, a credential source with no name.
	[[nodiscard]] auto validate( const provider_descriptor& descriptor ) -> status;

	// Parses the Lua-facing table form. Unknown keys are rejected, matching the
	// manifest rule (`19`): a typo that silently disables a field is worse than a
	// load error.
	[[nodiscard]] auto descriptor_from_json( std::string_view json_text )
		-> result< provider_descriptor >;

}
