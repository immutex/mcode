#include "mcode/mcp/supervisor.hxx"

#include "mcode/fs/workspace.hxx"
#include "mcode/support/logging.hxx"

#include <thread>
#include <utility>

namespace mcode::mcp {

	auto to_string( const server_state value ) noexcept -> std::string_view {
		switch ( value ) {
			case server_state::starting: return "starting";
			case server_state::ready: return "ready";
			case server_state::restarting: return "restarting";
			case server_state::failed: return "failed";
			case server_state::stopped: return "stopped";
		}

		return "unknown";
	}

	supervisor::supervisor( server_config config, handlers callbacks )
		: config_( std::move( config ) ), handlers_( std::move( callbacks ) ) { }

	supervisor::~supervisor( ) {
		shutdown( );
	}

	auto supervisor::connect( ) -> result< std::vector< server_tool > > {
		auto spawned = stdio_transport::spawn( config_ );

		if ( !spawned ) {
			return std::unexpected( std::move( spawned ).error( ) );
		}

		transport_ = std::unique_ptr< stdio_transport >(
			new stdio_transport( std::move( *spawned ) ) );
		client_ = std::make_unique< client >( *transport_ );

		client_->set_notify_handler( [ this ]( const jsonrpc::message& ) {
			// No server-initiated surface is advertised, so notifications carry
			// nothing this slice acts on. They are received and dropped here,
			// which is one place rather than scattered across the client.
		} );

		client_->attach( );

		auto caps = client_->initialize( );

		if ( !caps ) {
			return std::unexpected( std::move( caps ).error( ) );
		}

		auto listed = client_->list_tools( );

		if ( !listed ) {
			return std::unexpected( std::move( listed ).error( ) );
		}

		// Hash-pin the canonicalized list body so a server that changes its
		// tools after approval is detected rather than silently re-registered.
		auto rendered = std::string{ };

		for ( const auto& tool : *listed ) {
			rendered += tool.name;
			rendered.push_back( '\x1f' );
			rendered += tool.description;
			rendered.push_back( '\x1f' );
			rendered += tool.schema_json;
			rendered.push_back( '\x1e' );
		}

		pinned_hash_ = hash_bytes( rendered );
		tools_ = std::move( *listed );

		return tools_;
	}

	auto supervisor::start( ) -> result< std::vector< server_tool > > {
		auto connected = connect( );

		if ( !connected ) {
			state_ = server_state::failed;
			last_error_ = connected.error( ).msg;

			return std::unexpected( std::move( connected ).error( ) );
		}

		state_ = server_state::ready;

		if ( handlers_.on_ready ) {
			handlers_.on_ready( tools_ );
		}

		return tools_;
	}

	auto supervisor::backoff_delay( ) const -> std::chrono::milliseconds {
		auto delay = RESTART_BACKOFF_BASE;

		for ( std::size_t step = 0; step < restarts_ && step < RESTART_BACKOFF_STEPS; ++step ) {
			delay *= 2;
		}

		return delay;
	}

	auto supervisor::handle_eof( ) -> bool {
		if ( state_ == server_state::stopped || state_ == server_state::failed ) {
			return false;
		}

		state_ = server_state::restarting;

		// A restart sleeps for the backoff, reconnects, re-initializes,
		// re-lists and re-registers. The old transport is destroyed first, and
		// its destructor runs the graceful-shutdown sequence on a child that is
		// already dead -- which is a no-op, not an error.
		if ( client_ ) {
			client_->on_eof( );
		}

		client_.reset( );
		transport_.reset( );

		const auto delay = backoff_delay( );
		std::this_thread::sleep_for( delay );

		auto reconnected = connect( );

		if ( !reconnected ) {
			++restarts_;

			last_error_ = reconnected.error( ).msg;

			if ( restarts_ >= RESTART_BACKOFF_STEPS ) {
				state_ = server_state::failed;

				if ( handlers_.on_gone ) {
					handlers_.on_gone( last_error_ );
				}

				return false;
			}

			return handle_eof( );
		}

		restarts_ = 0;
		state_ = server_state::ready;

		if ( handlers_.on_ready ) {
			handlers_.on_ready( tools_ );
		}

		return true;
	}

	auto supervisor::on_transport_eof( ) -> void {
		handle_eof( );
	}

	auto supervisor::shutdown( ) -> void {
		if ( state_ == server_state::stopped ) {
			return;
		}

		if ( client_ ) {
			client_->on_eof( );
		}

		if ( transport_ ) {
			auto closed = transport_->close_input( );

			if ( !closed && logging_initialized( ) ) {
				logger( )->warn( "mcp: stdin close failed: {}", closed.error( ).msg );
			}

			transport_->finalize( );
			transport_->stop( );
		}

		state_ = server_state::stopped;
	}

	auto supervisor::pump( const std::chrono::milliseconds window ) -> void {
		if ( transport_ ) {
			transport_->pump( window );
		}
	}

	auto supervisor::detect_changed_tools( const std::vector< server_tool >& fresh ) const
		-> bool {
		auto rendered = std::string{ };

		for ( const auto& tool : fresh ) {
			rendered += tool.name;
			rendered.push_back( '\x1f' );
			rendered += tool.description;
			rendered.push_back( '\x1f' );
			rendered += tool.schema_json;
			rendered.push_back( '\x1e' );
		}

		return hash_bytes( rendered ) != pinned_hash_;
	}

	auto supervisor::report( ) const -> server_report {
		auto item = server_report{ };
		item.state = state_;
		item.last_error = last_error_;
		item.restarts = restarts_;

		if ( transport_ ) {
			item.stderr_text = transport_->stderr_text( );
		}

		return item;
	}

}
