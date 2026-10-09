#include "mcode/mcp/transport_stdio.hxx"

#include "mcode/mcp/jsonrpc.hxx"
#include "mcode/proc/process.hxx"

#include <utility>

namespace mcode::mcp {

	namespace {

		inline constexpr char LINE_TERMINATOR = '\n';

	}

	auto stdio_transport::spawn( const server_config& config ) -> result< stdio_transport > {
		if ( config.command.empty( ) ) {
			return std::unexpected( fail( errc::config,
				"server " + config.name + " has an empty command" ) );
		}

		if ( config.transport != "stdio" ) {
			return std::unexpected( fail( errc::unsupported,
				"transport " + config.transport + " is not supported; stdio only" ) );
		}

		auto executable = config.command.front( );

		// only a bare name is looked up on PATH: an absolute path fails on some platforms
		const auto has_directory = executable.find( '/' ) != std::string::npos ||
			executable.find( '\\' ) != std::string::npos;

		if ( !has_directory ) {
			if ( auto found = find_executable( executable ) ) {
				executable = std::move( *found );
			} else {
				return std::unexpected( std::move( found ).error( ) );
			}
		}

		auto options = proc::session_options{ };
		options.executable = std::move( executable );
		options.args.assign( config.command.begin( ) + 1, config.command.end( ) );

		auto child = proc::session::spawn( options );

		if ( !child ) {
			return std::unexpected( std::move( child ).error( ) );
		}

		return stdio_transport{ std::move( *child ) };
	}

	auto stdio_transport::send( const std::string_view frame_json ) -> status {
		if ( eof_seen_ ) {
			return std::unexpected( fail( errc::io, "the server's stdout has ended" ) );
		}

		auto line = std::string{ frame_json };
		line.push_back( LINE_TERMINATOR );

		return child_.write( line );
	}

	auto stdio_transport::emit_line( std::string line ) -> void {
		if ( !line.empty( ) && line.back( ) == '\r' ) {
			line.pop_back( );
		}

		auto item = inbound{ };
		item.skipped = false;
		item.line_json = std::move( line );

		if ( on_message ) {
			on_message( std::move( item ) );
		}
	}

	auto stdio_transport::dispatch_lines( const std::string_view chunk ) -> void {
		pending_.append( chunk );

		while ( true ) {
			const auto newline = pending_.find( LINE_TERMINATOR );

			if ( newline == std::string::npos ) {
				break;
			}

			auto line = std::string{ pending_.substr( 0, newline ) };
			pending_.erase( 0, newline + 1 );

			emit_line( std::move( line ) );
		}

		// one unterminated line cannot grow past a frame, or a hostile server exhausts memory
		if ( pending_.size( ) > jsonrpc::MAX_FRAME_BYTES ) {
			fail_oversized( );
		}
	}

	auto stdio_transport::flush_pending( ) -> void {
		if ( pending_.empty( ) ) {
			return;
		}

		auto line = std::move( pending_ );
		pending_.clear( );

		emit_line( std::move( line ) );
	}

	auto stdio_transport::fail_oversized( ) -> void {
		pending_.clear( );
		eof_seen_ = true;

		auto item = inbound{ };
		item.skipped = true;
		item.line_json = "transport: a frame exceeded the size bound";

		if ( on_message ) {
			on_message( std::move( item ) );
		}

		if ( on_eof ) {
			on_eof( );
		}
	}

	// The one place an EOF or a dead child is turned into the end of the stream. Both
	// loops call it, so both report the same reason and both flush the same partial
	// frame.
	auto stdio_transport::report_end( const std::string_view detail ) -> void {
		eof_seen_ = true;
		flush_pending( );

		if ( !detail.empty( ) ) {
			auto tail = inbound{ };
			tail.skipped = true;
			tail.line_json = "transport: " + std::string{ detail };

			if ( on_message ) {
				on_message( std::move( tail ) );
			}
		}

		if ( on_eof ) {
			on_eof( );
		}
	}

	auto stdio_transport::handle_read( const proc::read_result& chunk ) -> read_effect {
		switch ( chunk.kind ) {
			case proc::read_kind::data: {
				dispatch_lines( chunk.data );

				return eof_seen_ ? read_effect::stop : read_effect::proceed;
			}

			case proc::read_kind::timeout: {
				return read_effect::proceed;
			}

			case proc::read_kind::eof: {
				report_end( chunk.detail );

				return read_effect::stop;
			}
		}

		return read_effect::proceed;
	}

	auto stdio_transport::run( ) -> status {
		if ( eof_seen_ ) {
			return std::unexpected( fail( errc::io, "the transport already ended" ) );
		}

		while ( true ) {
			auto chunk = child_.read_some( std::chrono::milliseconds{ 50 } );

			if ( !chunk ) {
				return std::unexpected( std::move( chunk ).error( ) );
			}

			switch ( handle_read( *chunk ) ) {
				case read_effect::proceed: break;

				case read_effect::stop:
				case read_effect::failed: return { };
			}

			if ( !child_.running( ) ) {
				report_end( { } );

				return { };
			}
		}
	}

	auto stdio_transport::pump( const std::chrono::milliseconds window ) -> void {
		if ( eof_seen_ ) {
			return;
		}

		const auto deadline = std::chrono::steady_clock::now( ) + window;

		while ( std::chrono::steady_clock::now( ) < deadline && !eof_seen_ ) {
			auto chunk = child_.read_some( std::chrono::milliseconds{ 10 } );

			if ( !chunk ) {
				report_end( chunk.error( ).msg );

				return;
			}

			if ( handle_read( *chunk ) == read_effect::stop ) {
				return;
			}

			if ( !child_.running( ) ) {
				report_end( { } );

				return;
			}
		}
	}

	auto stdio_transport::close_input( ) -> status {
		return child_.close_stdin( );
	}

	auto stdio_transport::wait_exit( const std::chrono::milliseconds timeout ) -> void {
		child_.wait_exit( timeout );
	}

	auto stdio_transport::stop( ) -> void {
		child_.terminate( );
	}

	auto stdio_transport::alive( ) -> bool {
		return !eof_seen_ && child_.running( );
	}

	auto stdio_transport::stderr_text( ) const -> std::string {
		return child_.stderr_text( );
	}

	auto stdio_transport::finalize( ) -> void {
		child_.finalize( );
	}

}
