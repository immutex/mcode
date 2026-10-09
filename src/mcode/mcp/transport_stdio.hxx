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

		auto wait_exit( std::chrono::milliseconds timeout ) -> void override;

		auto stop( ) -> void override;

		auto alive( ) -> bool override;

		[[nodiscard]] auto stderr_text( ) const -> std::string override;

		auto finalize( ) -> void override;

	private:
		auto dispatch_lines( std::string_view chunk ) -> void;

		auto emit_line( std::string line ) -> void;

		// a residual frame without a trailing newline is still a frame
		auto flush_pending( ) -> void;

		auto fail_oversized( ) -> void;

		// What one read did to the loop. `run` and `pump` were the same loop
		// written twice, and they had already drifted: `pump` dropped the
		// OS-level reason an EOF carried, so a transport that died during a
		// `pump` reported nothing.
		enum class read_effect {
			proceed,
			stop,
			failed,
		};

		[[nodiscard]] auto handle_read( const proc::read_result& chunk ) -> read_effect;

		auto report_end( const std::string_view detail ) -> void;

		proc::session child_;
		std::string pending_;
		bool eof_seen_ = false;
	};

}
