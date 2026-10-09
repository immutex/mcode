#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::mcp::jsonrpc {

	// a hostile or broken server can send one enormous line; the transport and the parser both bound it
	inline constexpr std::size_t MAX_FRAME_BYTES = 8u * 1024u * 1024u;

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

	// a string id correlates by text, so "7" and 7 never collide on one pending call
	struct request_id {
		bool is_string = false;
		std::uint64_t number = 0;
		std::string text;

		[[nodiscard]] static auto numeric( const std::uint64_t value ) -> request_id;
		[[nodiscard]] static auto string( std::string value ) -> request_id;

		friend auto operator<=>( const request_id&, const request_id& ) = default;
	};

	// which field pair applies is decided by kind
	struct message {
		message_kind kind = message_kind::notification;

		request_id id;

		std::string method;
		std::string params_json;

		bool is_error = false;
		std::string body_json;
	};

	// the migration choke point: protocol version and client caps get injected here
	[[nodiscard]] auto stamp_meta( const std::string_view params_json ) -> result< std::string >;

	[[nodiscard]] auto render_request( const std::uint64_t id, const std::string_view method,
		const std::string_view params_json ) -> result< std::string >;

	[[nodiscard]] auto render_notification( const std::string_view method,
		const std::string_view params_json ) -> result< std::string >;

	[[nodiscard]] auto render_response( const request_id& id, const std::string_view result_json )
		-> result< std::string >;

	// a reply that carries `error` must not also carry `result`
	[[nodiscard]] auto render_error_response( const request_id& id, const int code,
		const std::string_view reason ) -> result< std::string >;

	// a non-JSON-RPC line (banner, blank, malformed) yields nothing, never an error
	[[nodiscard]] auto parse_line( const std::string_view line )
		-> result< std::optional< message > >;

}
