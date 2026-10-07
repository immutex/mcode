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

	[[nodiscard]] auto make_error( std::string_view message, std::string_view hint,
		bool retryable ) -> tool_error;

	[[nodiscard]] auto render_error( const tool_error& failure ) -> std::string;

	[[nodiscard]] auto error_result( std::string_view message, std::string_view hint,
		bool retryable ) -> std::string;

	// `detail` names the parameter, what was observed, and the admissible values where the
	// schema bounds them; `hint` is the recovery instruction. Both are model-facing.
	[[nodiscard]] auto argument_error( std::string_view detail,
		std::string_view hint ) -> std::string;

	// A call whose arguments were cut off by the output-token limit. Distinct from a malformed
	// call: nothing about the arguments is wrong, there is simply not enough of them, and the
	// recovery is to emit a smaller response rather than to correct a parameter.
	[[nodiscard]] auto truncation_error( std::string_view tool_name ) -> std::string;

}
