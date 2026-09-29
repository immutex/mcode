#include "mcode/tools/errors.hxx"

#include "mcode/support/json.hxx"

namespace mcode::tools {

	auto make_error( const std::string_view message, const std::string_view hint,
		const bool retryable ) -> tool_error {
		return tool_error{ .message = std::string{ message },
			.hint = std::string{ hint },
			.retryable = retryable };
	}

	auto render_error( const tool_error& failure ) -> std::string {
		auto out = std::string{ "{\"ok\":false,\"error\":\"" };
		json::append_escaped( out, failure.message );
		out += "\",\"hint\":\"";
		json::append_escaped( out, failure.hint );
		out += "\",\"retryable\":";
		out += failure.retryable ? "true" : "false";
		out += "}";

		return out;
	}

	auto error_result( const std::string_view message, const std::string_view hint,
		const bool retryable ) -> std::string {
		return render_error( make_error( message, hint, retryable ) );
	}

	auto argument_error( const std::string_view detail ) -> std::string {
		return error_result( std::string{ "malformed arguments: " } + std::string{ detail },
			"send a JSON object matching the tool's schema; re-read the schema with tool_search if unsure",
			false );
	}

}
