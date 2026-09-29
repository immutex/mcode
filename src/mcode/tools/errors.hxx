#pragma once

#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::tools {

	// The error contract every tool failure returns:
	// { "ok": false, "error": "<what failed>", "hint": "<specific fix>", "retryable": bool }
	//
	// Error text is written like advice to a colleague, including the fix. The
	// shape is one implementation, not eight hand-rolled copies.
	struct tool_error {
		std::string message;
		std::string hint;
		bool retryable = false;
	};

	[[nodiscard]] auto make_error( std::string_view message, std::string_view hint,
		bool retryable ) -> tool_error;

	// Renders the contract as the JSON string a handler returns on failure. The
	// loop surfaces handler errors as value-level failures; returning the JSON
	// keeps the contract in-band so the model can parse it.
	[[nodiscard]] auto render_error( const tool_error& failure ) -> std::string;

	// A handler result that carries a structured failure. Tools return this as a
	// normal string result; the `ok` field is what makes it honest.
	[[nodiscard]] auto error_result( std::string_view message, std::string_view hint,
		bool retryable ) -> std::string;

	// Parses a tool argument object, returning the error JSON on any failure.
	// Shared so a malformed argument gets the same contract shape everywhere.
	[[nodiscard]] auto argument_error( std::string_view detail ) -> std::string;

}
