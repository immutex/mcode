#pragma once

#include "mcode/mcp/constants.hxx"
#include "mcode/mcp/transport.hxx"
#include "mcode/proc/session.hxx"
#include "mcode/support/mcp_config.hxx"

namespace mcode::mcp {

	// The stdio transport: newline-delimited JSON-RPC over a child process's
	// stdin and stdout, with the child owned through a `proc::session`.
	//
	// The line splitter is fed every chunk `session::read_some` returns and
	// hands complete lines to the client. A non-JSON-RPC line is forwarded as
	// `skipped` -- the client logs it and moves on, because real servers print
	// banners and treating one as fatal would break servers that work everywhere
	// else.
	class stdio_transport final : public transport {
	public:
		explicit stdio_transport( proc::session child ) : child_( std::move( child ) ) { }

		stdio_transport( stdio_transport&& other ) noexcept = default;
		auto operator=( stdio_transport&& other ) noexcept -> stdio_transport& = default;

		// Spawns the server. `command[ 0 ]` is the executable, the rest the
		// argument vector, passed to the OS without a shell in between.
		[[nodiscard]] static auto spawn( const server_config& config ) -> result< stdio_transport >;

		auto send( std::string_view frame_json ) -> status override;

		auto run( ) -> status override;

		auto pump( std::chrono::milliseconds window ) -> void override;

		auto close_input( ) -> status override;

		auto stop( ) -> void override;

		auto alive( ) -> bool override;

		[[nodiscard]] auto stderr_text( ) const -> std::string override;

		auto finalize( ) -> void override;

	private:
		auto dispatch_lines( std::string_view chunk ) -> void;

		proc::session child_;
		std::string pending_;
		bool eof_seen_ = false;
	};

}
