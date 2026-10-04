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

	struct server_report {
		server_state state = server_state::starting;
		std::string last_error;

		// restart attempts since the last success
		std::size_t restarts = 0;

		std::string stderr_text;
	};

	class supervisor {
	public:
		// on_ready after every successful (re)connect; on_gone when the restart budget is spent
		struct handlers {
			std::function< void( const std::vector< server_tool >& ) > on_ready;
			std::function< void( const std::string& reason ) > on_gone;
		};

		supervisor( server_config config, handlers callbacks );
		~supervisor( );

		supervisor( const supervisor& ) = delete;
		auto operator=( const supervisor& ) -> supervisor& = delete;

		// fails closed: a server that cannot start is an error, not a silent absence
		auto start( ) -> result< std::vector< server_tool > >;

		// the transport's EOF callback: records a due restart, never resets the transport inline
		auto on_transport_eof( ) -> void;

		// close stdin, wait, terminate, finalize; the destructor runs this too
		auto shutdown( ) -> void;

		// services a due restart, then drains; the call path pumps through the client
		auto pump( std::chrono::milliseconds window ) -> void;

		[[nodiscard]] auto config( ) const noexcept -> const server_config& { return config_; }
		[[nodiscard]] auto state( ) const noexcept -> server_state { return state_; }
		[[nodiscard]] auto restarts( ) const noexcept -> std::size_t { return restarts_; }

		[[nodiscard]] auto pinned_hash( ) const noexcept -> const std::string& {
			return pinned_hash_;
		}

		[[nodiscard]] auto tools( ) const noexcept -> const std::vector< server_tool >& {
			return tools_;
		}

		// mismatch is reported, not silently accepted
		[[nodiscard]] auto detect_changed_tools( const std::vector< server_tool >& fresh )
			const -> bool;

		[[nodiscard]] auto report( ) const -> server_report;

		[[nodiscard]] auto client_ptr( ) noexcept -> client* { return client_.get( ); }

	private:
		auto connect( ) -> result< std::vector< server_tool > >;
		auto backoff_delay( ) const -> std::chrono::milliseconds;
		auto attempt_restart( ) -> void;

		server_config config_;
		handlers handlers_;

		std::unique_ptr< stdio_transport > transport_;
		std::unique_ptr< client > client_;

		server_state state_ = server_state::starting;
		std::size_t restarts_ = 0;
		std::string last_error_;
		std::string pinned_hash_;
		std::vector< server_tool > tools_;

		bool restart_pending_ = false;
		std::chrono::steady_clock::time_point next_attempt_{ };
	};

}
