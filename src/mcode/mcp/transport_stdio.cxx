#include "mcode/mcp/transport_stdio.hxx"

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

	auto stdio_transport::dispatch_lines( const std::string_view chunk ) -> void {
		pending_.append( chunk );

		while ( true ) {
			const auto newline = pending_.find( LINE_TERMINATOR );

			if ( newline == std::string::npos ) {
				break;
			}

			auto line = std::string{ pending_.substr( 0, newline ) };

			if ( !line.empty( ) && line.back( ) == '\r' ) {
				line.pop_back( );
			}

			pending_.erase( 0, newline + 1 );

			auto item = inbound{ };
			item.skipped = false;
			item.line_json = std::move( line );

			if ( on_message ) {
				on_message( std::move( item ) );
			}
		}
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

			switch ( chunk->kind ) {
				case proc::read_kind::data: {
					dispatch_lines( chunk->data );

					break;
				}

				case proc::read_kind::timeout: {
					break;
				}

				case proc::read_kind::eof: {
					eof_seen_ = true;

					if ( !chunk->detail.empty( ) ) {
						auto tail = inbound{ };
						tail.skipped = true;
						tail.line_json = "transport: " + chunk->detail;

						if ( on_message ) {
							on_message( std::move( tail ) );
						}
					}

					if ( on_eof ) {
						on_eof( );
					}

					return { };
				}
			}

			if ( !child_.running( ) ) {
				eof_seen_ = true;

				if ( on_eof ) {
					on_eof( );
				}

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
				eof_seen_ = true;

				if ( on_eof ) {
					on_eof( );
				}

				return;
			}

			switch ( chunk->kind ) {
				case proc::read_kind::data: {
					dispatch_lines( chunk->data );

					break;
				}

				case proc::read_kind::timeout: {
					break;
				}

				case proc::read_kind::eof: {
					eof_seen_ = true;

					if ( on_eof ) {
						on_eof( );
					}

					return;
				}
			}

			if ( !child_.running( ) ) {
				eof_seen_ = true;

				if ( on_eof ) {
					on_eof( );
				}

				return;
			}
		}
	}

	auto stdio_transport::close_input( ) -> status {
		return child_.close_stdin( );
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
