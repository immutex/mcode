#include "mcode/mcp/supervisor.hxx"

#include "mcode/fs/workspace.hxx"
#include "mcode/support/logging.hxx"

#include <utility>

namespace mcode::mcp {

	namespace {

		// one rendering for both the pin and the comparison, or a change could go undetected
		[[nodiscard]] auto render_tool_list( const std::vector< server_tool >& tools )
			-> std::string {
			auto out = std::string{ };

			for ( const auto& tool : tools ) {
				out += tool.name;
				out.push_back( '\x1f' );
				out += tool.description;
				out.push_back( '\x1f' );
				out += tool.schema_json;
				out.push_back( '\x1e' );
			}

			return out;
		}

	}

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

		client_->attach( );
		client_->set_end_handler( [ this ] { on_transport_eof( ); } );

		auto caps = client_->initialize( );

		if ( !caps ) {
			return std::unexpected( std::move( caps ).error( ) );
		}

		auto listed = client_->list_tools( );

		if ( !listed ) {
			return std::unexpected( std::move( listed ).error( ) );
		}

		// so a server that changes its tools after approval is detected, not silently re-registered
		pinned_hash_ = hash_bytes( render_tool_list( *listed ) );
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

	auto supervisor::attempt_restart( ) -> void {
		state_ = server_state::restarting;

		client_.reset( );
		transport_.reset( );

		auto reconnected = connect( );

		if ( reconnected ) {
			restarts_ = 0;
			state_ = server_state::ready;

			if ( handlers_.on_ready ) {
				handlers_.on_ready( tools_ );
			}

			return;
		}

		++restarts_;
		last_error_ = reconnected.error( ).msg;

		if ( restarts_ >= RESTART_BACKOFF_STEPS ) {
			state_ = server_state::failed;

			if ( handlers_.on_gone ) {
				handlers_.on_gone( last_error_ );
			}

			return;
		}

		// retried on the next pump after the backoff, so the caller is never blocked here
		restart_pending_ = true;
		next_attempt_ = std::chrono::steady_clock::now( ) + backoff_delay( );
	}

	auto supervisor::on_transport_eof( ) -> void {
		if ( state_ == server_state::stopped || state_ == server_state::failed ) {
			return;
		}

		if ( restart_pending_ ) {
			return;
		}

		restart_pending_ = true;
		next_attempt_ = std::chrono::steady_clock::now( );
	}

	auto supervisor::shutdown( ) -> void {
		if ( state_ == server_state::stopped ) {
			return;
		}

		state_ = server_state::stopped;
		restart_pending_ = false;

		if ( client_ ) {
			client_->on_eof( );
		}

		if ( transport_ ) {
			auto closed = transport_->close_input( );

			if ( !closed && logging_initialized( ) ) {
				logger( )->warn( "mcp: stdin close failed: {}", closed.error( ).msg );
			}

			// the drainer joins only once the child is dead, or a server ignoring EOF hangs it
			transport_->wait_exit( proc::SESSION_GRACE_WAIT );
			transport_->stop( );
			transport_->finalize( );
		}
	}

	auto supervisor::pump( const std::chrono::milliseconds window ) -> void {
		if ( restart_pending_ ) {
			if ( std::chrono::steady_clock::now( ) < next_attempt_ ) {
				return;
			}

			restart_pending_ = false;
			attempt_restart( );

			return;
		}

		if ( transport_ ) {
			transport_->pump( window );
		}
	}

	auto supervisor::detect_changed_tools( const std::vector< server_tool >& fresh ) const
		-> bool {
		return hash_bytes( render_tool_list( fresh ) ) != pinned_hash_;
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
