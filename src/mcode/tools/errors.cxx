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

	auto argument_error( const std::string_view detail, const std::string_view hint ) -> std::string {
		return error_result( std::string{ "invalid tool arguments: " } + std::string{ detail },
			hint, false );
	}

	auto truncation_error( const std::string_view tool_name ) -> std::string {
		return error_result(
			"the response was cut off by the output token limit before the arguments for '" +
				std::string{ tool_name } + "' were complete, so the call could not be run",
			"re-issue the call with shorter arguments, or split the work into several smaller "
			"calls; a lower max output or a smaller file window will leave room for the call",
			true );
	}

	auto loop_failure( const std::string_view tool_name, const std::string_view error_code )
		-> std::string {
		auto out = std::string{ "{\"ok\":false,\"error\":\"" };
		json::append_escaped( out, error_code );
		out += "\",\"tool\":\"";
		json::append_escaped( out, tool_name );
		out += "\"}";

		return out;
	}

	auto loop_denial( const denial_payload& request ) -> std::string {
		auto out = std::string{ "{\"ok\":false,\"denied\":true" };

		if ( request.veto ) {
			out += ",\"veto\":true";
		}

		out += ",\"tool\":\"";
		json::append_escaped( out, request.tool_name );
		out += "\",\"";
		out += request.detail_key;
		out += "\":\"";
		json::append_escaped( out, request.detail );
		out += "\",\"reason\":\"";
		json::append_escaped( out, request.reason );
		out += "\"}";

		return out;
	}

}
