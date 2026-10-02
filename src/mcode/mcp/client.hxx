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

	struct call_outcome {
		std::string content;

		// verbatim JSON, for callers that need the structured form
		std::string result_json;

		// isError is a tool-level result, not a protocol error
		bool is_error = false;
	};

	enum class failure_kind { none, transport, timeout, protocol };

	struct client_failure {
		failure_kind kind = failure_kind::none;
		std::string message;
	};

	using notify_callback = std::function< void( const jsonrpc::message& ) >;

	// every outgoing request goes through `call`, which stamps `_meta`
	class client {
	public:
		explicit client( transport& wire ) : wire_( &wire ) { }

		auto attach( ) -> void;

		auto set_notify_handler( notify_callback on_notify ) -> void {
			on_notify_ = std::move( on_notify );
		}

		// initialize is not cancellable, so it gets the absolute maximum, not the list timeout
		auto initialize( ) -> result< server_capabilities >;

		// follows nextCursor to completion; names are the server's own, unprefixed
		[[nodiscard]] auto list_tools( ) -> result< std::vector< server_tool > >;

		// a timeout sends notifications/cancelled, and a late response is ignored
		auto call_tool( std::string_view tool_name, std::string_view arguments_json,
			std::chrono::milliseconds timeout ) -> result< call_outcome >;

		auto call( std::string_view method, std::string_view params_json,
			std::chrono::milliseconds timeout ) -> result< std::string >;

		[[nodiscard]] auto capabilities( ) const noexcept -> const server_capabilities& {
			return caps_;
		}

		// after EOF every call is refused rather than written into a dead pipe
		[[nodiscard]] auto is_alive( ) const noexcept -> bool { return alive_; }

		// pending calls are failed, never replayed blind: a tool call may not be idempotent
		auto on_eof( ) -> void;

		[[nodiscard]] auto last_failure( ) const -> client_failure { return failure_; }

		// a late response after a timeout, or a duplicate after EOF
		[[nodiscard]] auto ignored_responses( ) const noexcept -> std::size_t {
			return ignored_responses_;
		}

		// the call path pumps while it waits, so the timeout fires at the call site
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
