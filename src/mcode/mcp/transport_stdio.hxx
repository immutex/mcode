#pragma once

#include "mcode/mcp/constants.hxx"
#include "mcode/mcp/transport.hxx"
#include "mcode/proc/session.hxx"
#include "mcode/support/mcp_config.hxx"

namespace mcode::mcp {

	// a non-JSON-RPC line is forwarded as `skipped`: real servers print banners, never fatal
	class stdio_transport final : public transport {
	public:
		explicit stdio_transport( proc::session child ) : child_( std::move( child ) ) { }

		stdio_transport( stdio_transport&& other ) noexcept = default;
		auto operator=( stdio_transport&& other ) noexcept -> stdio_transport& = default;

		// no shell: command[0] is the executable, the rest the argv
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
