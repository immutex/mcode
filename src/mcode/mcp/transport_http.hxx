#pragma once

#include <deque>
#include <string>

#include "mcode/mcp/transport.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/support/mcp_config.hxx"

namespace mcode::mcp {

	// MCP over Streamable HTTP (2025-06-18): one endpoint, POST per message, the
	// response either `application/json` or a `text/event-stream`.
	//
	// The minimal profile, deliberately: no GET listening stream. That stream exists
	// for server-initiated requests, and a client that does not advertise sampling
	// or elicitation will not receive any. Skipping it removes a long-lived
	// connection and its failure modes for no loss of capability.
	class http_transport final : public transport {
	public:
		explicit http_transport( server_config config );

		http_transport( http_transport&& other ) noexcept = default;
		auto operator=( http_transport&& other ) noexcept -> http_transport& = default;

		// Refuses a config with no usable url rather than failing on first send.
		[[nodiscard]] static auto create( const server_config& config )
			-> result< http_transport >;

		auto send( const std::string_view frame_json ) -> status override;

		auto run( ) -> status override;

		auto pump( std::chrono::milliseconds window ) -> void override;

		// Nothing to close: the transport is request/response, so this ends the
		// session by marking it dead and letting the supervisor's ladder take over.
		auto close_input( ) -> status override;

		auto wait_exit( std::chrono::milliseconds timeout ) -> void override;

		auto stop( ) -> void override;

		auto alive( ) -> bool override;

		// Streamable HTTP has no stderr channel; the last transport error is what a
		// reader needs instead, so it is reported here.
		[[nodiscard]] auto stderr_text( ) const -> std::string override;

		auto finalize( ) -> void override;

		// Sends `notifications/initialized` on the first successful POST, which the
		// streamable profile expects immediately after the handshake.
		[[nodiscard]] auto session_id( ) const -> std::string_view;

	private:
		auto post_one( const std::string& frame ) -> status;
		auto dispatch_body( const std::string& body, const std::string& content_type ) -> void;
		auto report_end( const std::string& detail ) -> void;

		server_config config_;
		mcode::net::http_client http_;

		std::deque< std::string > pending_;
		std::string session_id_;
		std::string last_error_;
		bool alive_ = true;
	};

}
