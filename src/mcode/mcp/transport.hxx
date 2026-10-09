#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"

namespace mcode::mcp {

	// skipped marks a line that was not a JSON-RPC frame; reported, never silently dropped
	struct inbound {
		bool skipped = false;
		std::string line_json;
	};

	// on_eof is an event, not an error return: restart policy and call rejection hang off it
	class transport {
	public:
		using message_callback = std::function< void( inbound&& ) >;
		using eof_callback = std::function< void( ) >;

		virtual ~transport( ) = default;

		transport( const transport& ) = delete;
		auto operator=( const transport& ) -> transport& = delete;

		virtual auto send( const std::string_view frame_json ) -> status = 0;

		// returns when the transport is dead; the callbacks have seen every line and the EOF
		virtual auto run( ) -> status = 0;

		// delivers what arrives within `window`, then returns; never blocks past it by a poll step
		virtual auto pump( std::chrono::milliseconds window ) -> void = 0;

		// ends the child's input without tearing the session down
		virtual auto close_input( ) -> status = 0;

		// gives the child up to `timeout` to exit on its own after close_input
		virtual auto wait_exit( std::chrono::milliseconds timeout ) -> void = 0;

		virtual auto stop( ) -> void = 0;

		// false after EOF even if the OS process object lingers
		virtual auto alive( ) -> bool = 0;

		// bounded ring: oldest dropped
		[[nodiscard]] virtual auto stderr_text( ) const -> std::string = 0;

		// joins background work, so the stderr ring's final content is deterministic
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
