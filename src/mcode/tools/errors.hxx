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

	// `detail` names the parameter, what was observed, and the admissible values where the
	// schema bounds them; `hint` is the recovery instruction. Both are model-facing.
	[[nodiscard]] auto argument_error( const std::string_view detail,
		const std::string_view hint ) -> std::string;

	// A call whose arguments were cut off by the output-token limit. Distinct from a malformed
	// call: nothing about the arguments is wrong, there is simply not enough of them, and the
	// recovery is to emit a smaller response rather than to correct a parameter.
	[[nodiscard]] auto truncation_error( const std::string_view tool_name ) -> std::string;

	// The loop's own failure envelopes: `{ "ok": false, "tool": name, "error": code }`.
	// Built by hand in eleven places, two of which were byte-identical, so a field
	// added to the shape reached some call sites and not others.
	[[nodiscard]] auto loop_failure( const std::string_view tool_name, const std::string_view error_code )
		-> std::string;

	// A denied call carries the flag the approval layer set, plus the optional veto.
	// The two paths name the refusing rule under different keys, and the record is
	// public, so the key is a parameter rather than a rename.
	struct denial_payload {
		std::string_view tool_name;
		bool veto = false;
		std::string_view detail_key;
		std::string_view detail;
		std::string_view reason;
	};

	[[nodiscard]] auto loop_denial( const denial_payload& request ) -> std::string;

}
