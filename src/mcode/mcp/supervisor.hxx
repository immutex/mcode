#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mcode/core/error.hxx"
#include "mcode/mcp/client.hxx"
#include "mcode/mcp/transport.hxx"
#include "mcode/mcp/transport_stdio.hxx"
#include "mcode/mcp/constants.hxx"

namespace mcode::mcp {

	enum class server_state { starting, ready, restarting, failed, stopped };

	[[nodiscard]] auto to_string( const server_state value ) noexcept -> std::string_view;

	// What a server reports to whoever watches it. The registry's tool list is
	// one consumer; a status view is another.
	struct server_report {
		server_state state = server_state::starting;
		std::string last_error;

		// The restart attempts consumed since the last success. Zero on a
		// server that has never crashed.
		std::size_t restarts = 0;

		// The bounded stderr ring, verbatim.
		std::string stderr_text;
	};

	// Owns one stdio server's process lifetime: spawn, EOF detection, restart
	// with capped backoff, graceful shutdown.
	//
	// The supervisor owns the transport and the client. On restart it re-runs
	// the handshake, re-lists the tools and calls `on_ready` again -- a
	// respawned server's tools come back, which is exactly the path Claude Code
	// got wrong by excluding stdio from reconnect entirely.
	class supervisor {
	public:
		// `on_ready` runs after every successful (re)connect with the fresh tool
		// list; `on_gone` runs when the restart budget is exhausted.
		struct handlers {
			std::function< void( const std::vector< server_tool >& ) > on_ready;
			std::function< void( const std::string& reason ) > on_gone;
		};

		supervisor( server_config config, handlers callbacks );
		~supervisor( );

		supervisor( const supervisor& ) = delete;
		auto operator=( const supervisor& ) -> supervisor& = delete;

		// Spawns and performs the first handshake + list. Fails closed: a
		// server that cannot start is an error, not a silent absence.
		auto start( ) -> result< std::vector< server_tool > >;

		// Marks the server failed and rejects every pending call with a
		// transport error. Pending calls are never replayed blind -- a tool call
		// may not be idempotent. Returns false when the restart budget is
		// exhausted, in which case the server stays down and `on_gone` fires.
		auto handle_eof( ) -> bool;

		// Graceful shutdown: close stdin, wait <=5s, terminate, wait <=2s, kill.
		// The destructor runs this too; an explicit call makes the sequencing
		// observable and testable.
		auto shutdown( ) -> void;

		// The transport's EOF event, wired from the transport. Drives
		// `handle_eof` and the restart attempt.
		auto on_transport_eof( ) -> void;

		// Drives the read loop for up to `window`. The call path pumps through
		// the client; this is the supervisor's own drain for background reads.
		auto pump( std::chrono::milliseconds window ) -> void;

		[[nodiscard]] auto config( ) const noexcept -> const server_config& { return config_; }
		[[nodiscard]] auto state( ) const noexcept -> server_state { return state_; }
		[[nodiscard]] auto restarts( ) const noexcept -> std::size_t { return restarts_; }

		// The pinned hash of the tools/list body. A server that changes its
		// tools after approval is detected here rather than silently
		// re-registered.
		[[nodiscard]] auto pinned_hash( ) const noexcept -> const std::string& {
			return pinned_hash_;
		}

		// The last `tools/list` result, for re-registration after a restart.
		[[nodiscard]] auto tools( ) const noexcept -> const std::vector< server_tool >& {
			return tools_;
		}

		// Compares a fresh list against the pin. Mismatch is reported, not
		// silently accepted -- the rug-pull defense.
		[[nodiscard]] auto detect_changed_tools( const std::vector< server_tool >& fresh )
			const -> bool;

		[[nodiscard]] auto report( ) const -> server_report;

		[[nodiscard]] auto client_ptr( ) noexcept -> client* { return client_.get( ); }

	private:
		auto connect( ) -> result< std::vector< server_tool > >;
		auto backoff_delay( ) const -> std::chrono::milliseconds;

		server_config config_;
		handlers handlers_;

		std::unique_ptr< stdio_transport > transport_;
		std::unique_ptr< client > client_;

		server_state state_ = server_state::starting;
		std::size_t restarts_ = 0;
		std::string last_error_;
		std::string pinned_hash_;
		std::vector< server_tool > tools_;
	};

}
