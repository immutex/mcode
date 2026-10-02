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

	[[nodiscard]] auto argument_error( std::string_view detail ) -> std::string;

}
