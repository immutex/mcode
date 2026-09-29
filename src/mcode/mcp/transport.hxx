#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::mcp {

	// One parsed line delivered to the client. `skipped` marks a line that was
	// not a JSON-RPC frame -- a banner, a blank line, a stray log line -- and is
	// reported rather than silently dropped, so the log shows what the server
	// actually printed.
	struct inbound {
		bool skipped = false;
		std::string line_json;
	};

	// The transport seam between the client and the wire.
	//
	// The client drives: it hands frames to `send` and receives lines through
	// `on_message`. `on_eof` is the event that marks the server gone -- the
	// supervisor's restart policy and the client's pending-call rejection both
	// hang off it, and it is an event, not an error return.
	class transport {
	public:
		using message_callback = std::function< void( inbound&& ) >;
		using eof_callback = std::function< void( ) >;

		virtual ~transport( ) = default;

		transport( const transport& ) = delete;
		auto operator=( const transport& ) -> transport& = delete;

		// One JSON-RPC frame, newline-terminated by the implementation.
		virtual auto send( std::string_view frame_json ) -> status = 0;

		// Runs until the server's stdout ends. Returns when the transport is
		// dead; the callbacks have seen every line and the EOF by then.
		virtual auto run( ) -> status = 0;

		// Drives the transport for up to `window`, delivering whatever arrives
		// through the callbacks, then returns. Never blocks past the window by
		// more than a poll step. The client's call path uses this to wait on a
		// specific response while keeping the timeout at the call site.
		virtual auto pump( std::chrono::milliseconds window ) -> void = 0;

		// Ends the child's input without tearing the session down. A graceful
		// shutdown asks the server to finish; `stop` is the harder cut.
		virtual auto close_input( ) -> status = 0;

		// Unconditional termination. The last step of the shutdown sequence.
		virtual auto stop( ) -> void = 0;

		// True while the child is alive. A transport that has seen EOF reports
		// false even if the OS process object lingers.
		virtual auto alive( ) -> bool = 0;

		// The bounded stderr ring: the most recent output, oldest dropped.
		[[nodiscard]] virtual auto stderr_text( ) const -> std::string = 0;

		// Joins any background work the transport started, so the stderr ring's
		// final content is deterministic for whoever reads it next.
		virtual auto finalize( ) -> void = 0;

	protected:
		transport( ) = default;
		transport( transport&& other ) noexcept = default;
		auto operator=( transport&& other ) noexcept -> transport& = default;

		message_callback on_message;
		eof_callback on_eof;

	public:
		auto set_callbacks( message_callback on_line, eof_callback on_end ) -> void {
			on_message = std::move( on_line );
			on_eof = std::move( on_end );
		}
	};

}
