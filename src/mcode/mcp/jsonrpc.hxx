#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::mcp::jsonrpc {

	inline constexpr int PARSE_ERROR = -32700;
	inline constexpr int INVALID_REQUEST = -32600;
	inline constexpr int METHOD_NOT_FOUND = -32601;
	inline constexpr int INVALID_PARAMS = -32602;
	inline constexpr int INTERNAL_ERROR = -32603;

	inline constexpr std::string_view INITIALIZE = "initialize";
	inline constexpr std::string_view INITIALIZED_NOTIFICATION = "notifications/initialized";
	inline constexpr std::string_view TOOLS_LIST = "tools/list";
	inline constexpr std::string_view TOOLS_CALL = "tools/call";
	inline constexpr std::string_view PING = "ping";
	inline constexpr std::string_view CANCELLED_NOTIFICATION = "notifications/cancelled";

	enum class message_kind { request, notification, response };

	// which field pair applies is decided by kind
	struct message {
		message_kind kind = message_kind::notification;

		std::uint64_t id = 0;

		std::string method;
		std::string params_json;

		bool is_error = false;
		std::string body_json;
	};

	// the migration choke point: protocol version and client caps get injected here
	[[nodiscard]] auto stamp_meta( std::string_view params_json ) -> result< std::string >;

	[[nodiscard]] auto render_request( std::uint64_t id, std::string_view method,
		std::string_view params_json ) -> result< std::string >;

	[[nodiscard]] auto render_notification( std::string_view method,
		std::string_view params_json ) -> result< std::string >;

	[[nodiscard]] auto render_response( std::uint64_t id, std::string_view result_json )
		-> result< std::string >;

	// a non-JSON-RPC line (banner, blank, malformed) yields nothing, never an error
	[[nodiscard]] auto parse_line( std::string_view line )
		-> result< std::optional< message > >;

}
