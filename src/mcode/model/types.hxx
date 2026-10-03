#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/core/error.hxx"

namespace mcode::model {

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

		std::string text;

		// args are fragments until the call completes, so an incomplete call has empty args_json.
		std::string tool_call_id;
		std::string tool_name;
		std::string args_json;

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

	enum class effort {
		provider_default,
		low,
		medium,
		high,
	};

	enum class cache_mode {
		implicit,
		explicit_markers,
		// not supported; the layout rule still applies, it just costs full price.
		none,
	};

	struct cache_plan {
		cache_mode mode = cache_mode::none;

		// Byte offsets into the rendered body; the renderer applies them right to left. The
		// assembler fills them, because only the rendered bytes know where the prefix ends.
		std::vector< std::size_t > breakpoints;
	};

	struct chat_request {
		std::string model;
		std::vector< message > messages;
		std::vector< tool_spec > tools;
		tool_choice choice;

		effort reasoning_effort = effort::provider_default;

		std::string response_schema_json;

		cache_plan cache;

		std::int64_t max_output_tokens = 0;
		double temperature = -1.0;
		bool stream = true;
	};

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

		std::string text;

		// the first fragment for an index carries id and name, later ones only args.
		int index = 0;
		std::string tool_call_id;
		std::string tool_name;
		std::string args_fragment;

		std::int64_t input_tokens = 0;
		std::int64_t output_tokens = 0;
		std::int64_t cached_read_tokens = 0;
		std::int64_t cache_write_tokens = 0;
		std::int64_t reasoning_tokens = 0;

		std::string stop_reason;

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

	// The window assumed for a model the table does not know; without one it never compacts.
	inline constexpr std::int64_t DEFAULT_CONTEXT_WINDOW = 200'000;

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

		// A model with no declared window still gets one, so the loop can budget against it.
		[[nodiscard]] constexpr auto effective_context_window( ) const noexcept -> std::int64_t {
			return context_window > 0 ? context_window : DEFAULT_CONTEXT_WINDOW;
		}
	};

	// computed from provider-reported usage only; an unknown model prices at zero.
	[[nodiscard]] auto compute_cost( const capabilities& caps, const usage& counts ) -> double;

}
