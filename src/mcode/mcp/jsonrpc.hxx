#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::mcp::jsonrpc {

	// JSON-RPC 2.0 error codes the client may receive.
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

	// One parsed frame. A request and a response share the id field, a request and
	// a notification share the method field; the kind says which pair applies.
	struct message {
		message_kind kind = message_kind::notification;

		// request + response
		std::uint64_t id = 0;

		// request + notification
		std::string method;
		std::string params_json;

		// response
		bool is_error = false;
		std::string body_json;
	};

	// Stamps the `_meta` map into a params object and re-renders it.
	//
	// This is the migration choke point: when the stateless protocol revision is
	// adopted, the protocol version and client capabilities are injected here and
	// the handshake becomes a no-op. Every outgoing request passes through it, so
	// no session state may live deeper than the client object.
	[[nodiscard]] auto stamp_meta( std::string_view params_json ) -> result< std::string >;

	// Renders one frame. The params are embedded verbatim; the caller stamps
	// `_meta` first when the frame is a request.
	[[nodiscard]] auto render_request( std::uint64_t id, std::string_view method,
		std::string_view params_json ) -> result< std::string >;

	// A notification with no params carries none at all.
	[[nodiscard]] auto render_notification( std::string_view method,
		std::string_view params_json ) -> result< std::string >;

	[[nodiscard]] auto render_response( std::uint64_t id, std::string_view result_json )
		-> result< std::string >;

	// Parses one line of newline-delimited JSON-RPC.
	//
	// A line that is not a JSON-RPC frame -- a startup banner, a blank line,
	// malformed JSON -- yields nothing rather than an error: real servers print
	// banners, and treating one as fatal would break servers that work everywhere
	// else. The caller logs the skip and moves on.
	[[nodiscard]] auto parse_line( std::string_view line )
		-> result< std::optional< message > >;

}
