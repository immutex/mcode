#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::model {

	// Canonical provider-neutral model. Providers are thin: translate a
	// request, parse a stream, own no policy. Everything here is a value type so
	// a request can be copied for a retry or a branch without aliasing.
	//
	// JSON is carried as text, not as a parsed tree. Providers embed it directly
	// into a request body, and the delta applier in D2 edits it in place; a
	// parsed form would only add a second representation to keep in sync.

	enum class role {
		system,
		user,
		assistant,
		tool,
	};

	[[nodiscard]] constexpr auto to_string( const role value ) noexcept -> std::string_view {
		switch ( value ) {
			case role::system: return "system";
			case role::user: return "user";
			case role::assistant: return "assistant";
			case role::tool: return "tool";
		}

		return "user";
	}

	[[nodiscard]] auto role_from_string( const std::string_view text ) -> std::optional< role >;

	enum class block_kind {
		text,
		thinking,
		tool_call,
		tool_result,
	};

	struct block {
		block_kind kind = block_kind::text;

		// text and thinking
		std::string text;

		// tool_call: the provider's call id, the tool name, and the arguments as
		// a JSON object. Tool arguments arrive as string fragments and are only
		// parsed once the call is complete, so an incomplete
		// call is represented by an empty args_json, never by partial JSON.
		std::string tool_call_id;
		std::string tool_name;
		std::string args_json;

		// tool_result
		std::string result_json;
		bool is_error = false;
	};

	struct message {
		role speaker = role::user;
		std::vector< block > blocks;

		[[nodiscard]] auto text( ) const -> std::string;
	};

	struct tool_spec {
		std::string name;
		std::string description;
		std::string schema_json;
	};

	enum class tool_choice_kind {
		automatic,
		none,
		required,
		specific,
	};

	struct tool_choice {
		tool_choice_kind kind = tool_choice_kind::automatic;
		std::string name;
	};

	// Reasoning effort. Providers that lack it ignore the field; the registry
	// says which levels a model accepts.
	enum class effort {
		provider_default,
		low,
		medium,
		high,
	};

	enum class cache_mode {
		// Provider does it automatically; nothing to send.
		implicit,
		// Requires explicit breakpoints.
		explicit_markers,
		// Not supported; the layout rule still applies, it just costs full price.
		none,
	};

	struct cache_plan {
		cache_mode mode = cache_mode::none;
		// Byte offsets into the rendered prompt where a cache breakpoint goes.
		std::vector< std::size_t > breakpoints;
	};

	struct chat_request {
		std::string model;
		std::vector< message > messages;
		std::vector< tool_spec > tools;
		tool_choice choice;

		effort reasoning_effort = effort::provider_default;

		// Structured output schema, or empty. Providers that cannot express it
		// must fail early rather than silently dropping the constraint.
		std::string response_schema_json;

		cache_plan cache;

		std::int64_t max_output_tokens = 0;
		double temperature = -1.0;
		bool stream = true;
	};

	// One normalized streaming event. The agent loop consumes only these.
	struct chat_event {
		enum class kind {
			text_delta,
			thinking_delta,
			tool_call_delta,
			usage,
			turn_done,
			error,
		};

		kind type = kind::text_delta;

		// text_delta and thinking_delta
		std::string text;

		// tool_call_delta. `index` distinguishes parallel calls; the first
		// fragment for an index carries id and name, later ones only args.
		int index = 0;
		std::string tool_call_id;
		std::string tool_name;
		std::string args_fragment;

		// usage
		std::int64_t input_tokens = 0;
		std::int64_t output_tokens = 0;
		std::int64_t cached_read_tokens = 0;
		std::int64_t cache_write_tokens = 0;
		std::int64_t reasoning_tokens = 0;

		// turn_done
		std::string stop_reason;

		// error
		errc code = errc::ok;
		std::string message_text;
		bool retryable = false;
	};

	struct usage {
		std::int64_t input = 0;
		std::int64_t output = 0;
		std::int64_t cached_read = 0;
		std::int64_t cache_write = 0;
		std::int64_t reasoning = 0;

		auto add( const chat_event& event ) -> void;
		[[nodiscard]] auto total_tokens( ) const noexcept -> std::int64_t;
	};

	// What a model accepts. Compiled-in JSON table, not code.
	struct capabilities {
		std::string model;

		cache_mode caching = cache_mode::none;
		bool supports_tool_calls = true;
		bool supports_strict_schema = false;
		bool supports_response_schema = false;
		bool supports_thinking = false;
		bool supports_effort = false;

		std::int64_t context_window = 0;
		std::int64_t max_output_tokens = 0;

		// USD per million tokens.
		double price_input = 0.0;
		double price_cached_read = 0.0;
		double price_cache_write = 0.0;
		double price_output = 0.0;
	};

	// Cost is computed from provider-reported usage only, never estimated from a
	// tokenizer. An unknown model prices at zero rather than guessing.
	[[nodiscard]] auto compute_cost( const capabilities& caps, const usage& counts ) -> double;

}
