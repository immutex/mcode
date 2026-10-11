#pragma once

#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::tools {

	// { "ok": false, "error": ..., "hint": ..., "retryable": bool }
	struct tool_error {
		std::string message;
		std::string hint;
		bool retryable = false;
	};

	[[nodiscard]] auto make_error( const std::string_view message, const std::string_view hint,
		const bool retryable ) -> tool_error;

	[[nodiscard]] auto render_error( const tool_error& failure ) -> std::string;

	[[nodiscard]] auto error_result( const std::string_view message, const std::string_view hint,
		const bool retryable ) -> std::string;

	// model-facing: name the parameter, what was observed, and the admissible values
	[[nodiscard]] auto argument_error( const std::string_view detail,
		const std::string_view hint ) -> std::string;

	// not a malformed call: the arguments are incomplete, so the recovery is a smaller response
	[[nodiscard]] auto truncation_error( const std::string_view tool_name ) -> std::string;

	// `{ "ok": false, "tool": name, "error": code }`
	[[nodiscard]] auto loop_failure( const std::string_view tool_name, const std::string_view error_code )
		-> std::string;

	// the two call paths name the refusing rule under different keys, so the key is a parameter
	struct denial_payload {
		std::string_view tool_name;
		bool veto = false;
		std::string_view detail_key;
		std::string_view detail;
		std::string_view reason;
	};

	[[nodiscard]] auto loop_denial( const denial_payload& request ) -> std::string;

}
