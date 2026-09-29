#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"
#include "mcode/mcp/jsonrpc.hxx"
#include "mcode/mcp/transport.hxx"

namespace mcode::mcp {

	struct server_tool {
		std::string name;
		std::string description;
		std::string schema_json;
	};

	struct server_capabilities {
		bool tools = false;
		std::string server_name;
		std::string server_version;
		std::string negotiated_version;
		std::string instructions;
	};

	// One completed `tools/call`.
	struct call_outcome {
		// The result's text content, when the server produced any.
		std::string content;

		// The raw result body, verbatim JSON, for callers that need the
		// structured form.
		std::string result_json;

		// A tool-level failure (`isError: true`) is a result, not a protocol
		// error; the caller distinguishes by this flag.
		bool is_error = false;
	};

	enum class failure_kind { none, transport, timeout, protocol };

	struct client_failure {
		failure_kind kind = failure_kind::none;
		std::string message;
	};

	using notify_callback = std::function< void( const jsonrpc::message& ) >;

	// The MCP client over one transport: handshake, correlated requests,
	// per-request timeouts, and the `_meta` choke point.
	//
	// Every outgoing call goes through `call`, which stamps `_meta` -- the
	// migration hedge. No session state lives deeper than this object.
	class client {
	public:
		explicit client( transport& wire ) : wire_( &wire ) { }

		// Wires the callbacks and returns the notify handler the transport's
		// `on_message` must call into. The transport's EOF path calls `on_eof`,
		// which fails every pending call and marks the client dead.
		auto attach( ) -> void;

		auto set_notify_handler( notify_callback on_notify ) -> void {
			on_notify_ = std::move( on_notify );
		}

		// The `initialize` handshake: request, verify the echoed protocol
		// version, record capabilities, send `notifications/initialized`.
		// `initialize` itself is not cancellable, so it gets the absolute
		// maximum rather than the list timeout.
		auto initialize( ) -> result< server_capabilities >;

		// Follows `nextCursor` until the list is complete. The returned tools
		// are the server's own names, unprefixed.
		[[nodiscard]] auto list_tools( ) -> result< std::vector< server_tool > >;

		// One `tools/call`. `arguments_json` is embedded verbatim as the
		// `arguments` object. Timeout applies to this request alone; a timeout
		// sends `notifications/cancelled`, and a late response is ignored.
		auto call_tool( std::string_view tool_name, std::string_view arguments_json,
			std::chrono::milliseconds timeout ) -> result< call_outcome >;

		// The one choke point every outgoing request passes through. Stamps
		// `_meta` into the params and correlates the response by id.
		auto call( std::string_view method, std::string_view params_json,
			std::chrono::milliseconds timeout ) -> result< std::string >;

		[[nodiscard]] auto capabilities( ) const noexcept -> const server_capabilities& {
			return caps_;
		}

		// True between a successful `initialize` and EOF. After EOF the client
		// refuses every call with a transport error rather than writing into a
		// dead pipe.
		[[nodiscard]] auto is_alive( ) const noexcept -> bool { return alive_; }

		// The transport-side EOF event. Fails every pending call with a
		// transport error. Pending calls are never replayed blind -- a tool call
		// may not be idempotent.
		auto on_eof( ) -> void;

		[[nodiscard]] auto last_failure( ) const -> client_failure { return failure_; }

		// True when a response for an id nobody waits on arrives. A late
		// response after a timeout, or a duplicate after EOF: observed, ignored,
		// never delivered as a second result.
		[[nodiscard]] auto ignored_responses( ) const noexcept -> std::size_t {
			return ignored_responses_;
		}

		// Drives the transport's read loop for up to `window`, delivering any
		// lines that arrive. The call path pumps while it waits, so a timeout
		// fires at the call site where the deadline is known and the loop is
		// never blocked by a silent server.
		auto pump( std::chrono::milliseconds window ) -> void;

	private:
		struct pending_call {
			std::string method;
			std::optional< std::string > response_json;
			std::optional< error > failure;
			bool done = false;
		};

		auto fail_pending( const errc code, std::string message ) -> void;
		auto handle_line( inbound&& item ) -> void;
		auto send_notification( std::string_view method, std::string_view params_json )
			-> status;

		transport* wire_ = nullptr;
		server_capabilities caps_;
		bool alive_ = false;
		bool eof_seen_ = false;

		std::uint64_t next_id_ = 1;
		std::map< std::uint64_t, pending_call > pending_;
		notify_callback on_notify_;

		client_failure failure_;
		std::size_t ignored_responses_ = 0;
	};

}
