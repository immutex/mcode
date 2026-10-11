#include "mcode/mcp/transport_http.hxx"

#include <algorithm>
#include <thread>

#include "mcode/mcp/constants.hxx"
#include "mcode/support/text.hxx"

namespace mcode::mcp {

	namespace {

		constexpr std::string_view JSON_CONTENT_TYPE = "application/json";
		constexpr std::string_view SSE_CONTENT_TYPE = "text/event-stream";
		constexpr std::string_view SESSION_HEADER = "mcp-session-id";
		constexpr std::string_view ACCEPT_VALUE = "application/json, text/event-stream";

		auto header_value( const std::map< std::string, std::string, std::less<> >& headers,
			const std::string_view name ) -> std::string {
			const auto wanted = text::ascii_lower( name );

			for ( const auto& [ key, value ] : headers ) {
				if ( text::ascii_lower( key ) == wanted ) {
					return value;
				}
			}

			return { };
		}

		auto is_json_content_type( const std::string& content_type ) -> bool {
			return text::ascii_lower( content_type ).find( JSON_CONTENT_TYPE )
				!= std::string::npos;
		}

		auto is_sse_content_type( const std::string& content_type ) -> bool {
			return text::ascii_lower( content_type ).find( SSE_CONTENT_TYPE ) != std::string::npos;
		}

		// One `data:` payload per SSE event; `event:` and `id:` carry nothing we act on.
		auto sse_data_frames( const std::string& body, std::vector< std::string >& out ) -> void {
			auto start = std::size_t{ 0 };

			while ( start < body.size( ) ) {
				const auto end = body.find( '\n', start );
				const auto stop = end == std::string::npos ? body.size( ) : end;
				auto line = std::string_view{ body }.substr( start, stop - start );

				if ( !line.empty( ) && line.back( ) == '\r' ) {
					line.remove_suffix( 1 );
				}

				if ( line.starts_with( "data:" ) ) {
					auto payload = line.substr( 5 );

					while ( !payload.empty( ) && payload.front( ) == ' ' ) {
						payload.remove_prefix( 1 );
					}

					if ( !payload.empty( ) ) {
						out.emplace_back( payload );
					}
				}

				if ( end == std::string::npos ) {
					break;
				}

				start = end + 1;
			}
		}

	}

	http_transport::http_transport( server_config config ) : config_( std::move( config ) ) { }

	auto http_transport::create( const server_config& config ) -> result< http_transport > {
		if ( config.url.empty( ) ) {
			return std::unexpected( fail( errc::config,
				"mcp server '" + config.name + "' uses a remote transport but declares no url" ) );
		}

		const auto parsed = mcode::net::parse_url( config.url );

		if ( !parsed ) {
			return std::unexpected( fail( errc::config,
				"mcp server '" + config.name + "' has an unparsable url: " + config.url ) );
		}

		if ( parsed->scheme != "http" && parsed->scheme != "https" ) {
			return std::unexpected( fail( errc::config,
				"mcp server '" + config.name + "' needs an http or https url, got '"
					+ parsed->scheme + "'" ) );
		}

		return http_transport{ config };
	}

	auto http_transport::session_id( ) const -> std::string_view {
		return session_id_;
	}

	auto http_transport::send( const std::string_view frame_json ) -> status {
		if ( !alive_ ) {
			return std::unexpected( fail( errc::protocol,
				"the transport to '" + config_.name + "' is no longer alive" ) );
		}

		pending_.emplace_back( frame_json );

		return { };
	}

	auto http_transport::post_one( const std::string& frame ) -> status {
		auto request = mcode::net::http_request{ };
		request.url = config_.url;
		request.method = "POST";
		request.body = frame;
		request.timeout_seconds = 120;
		request.headers.emplace( "Content-Type", std::string{ JSON_CONTENT_TYPE } );
		request.headers.emplace( "Accept", std::string{ ACCEPT_VALUE } );
		request.headers.emplace( "MCP-Protocol-Version", std::string{ PROTOCOL_VERSION } );

		if ( !session_id_.empty( ) ) {
			request.headers.emplace( "MCP-Session-Id", session_id_ );
		}

		auto response = http_.send( request );

		if ( !response ) {
			last_error_ = response.error( ).msg;
			alive_ = false;

			return std::unexpected( response.error( ) );
		}

		// The session id is issued on `initialize` and echoed later; a 404 means the
		// server forgot it, which the caller re-initializes from.
		if ( const auto issued = header_value( response->headers, SESSION_HEADER );
			!issued.empty( ) ) {
			session_id_ = issued;
		}

		if ( response->status == 404 && !session_id_.empty( ) ) {
			session_id_.clear( );
		}

		if ( response->status < 200 || response->status >= 300 ) {
			last_error_ = "HTTP " + std::to_string( response->status );

			return std::unexpected( fail( errc::protocol, last_error_ ) );
		}

		// A notification is answered with no body, so there is nothing to dispatch.
		if ( response->body.empty( ) ) {
			return { };
		}

		dispatch_body( response->body, header_value( response->headers, "content-type" ) );

		return { };
	}

	auto http_transport::dispatch_body( const std::string& body,
		const std::string& content_type ) -> void {
		if ( is_sse_content_type( content_type ) ) {
			auto frames = std::vector< std::string >{ };

			sse_data_frames( body, frames );

			for ( auto& frame : frames ) {
				if ( on_message ) {
					on_message( inbound{ .skipped = false, .line_json = std::move( frame ) } );
				}
			}

			return;
		}

		// A body that is neither content type is reported as skipped rather than
		// dropped, so a server speaking something else stays visible.
		if ( is_json_content_type( content_type ) || body.starts_with( '{' ) ) {
			if ( on_message ) {
				on_message( inbound{ .skipped = false, .line_json = body } );
			}

			return;
		}

		if ( on_message ) {
			on_message( inbound{ .skipped = true, .line_json = body } );
		}
	}

	auto http_transport::pump( const std::chrono::milliseconds window ) -> void {
		if ( !alive_ ) {
			return;
		}

		const auto deadline = std::chrono::steady_clock::now( ) + window;

		while ( !pending_.empty( ) ) {
			auto frame = std::move( pending_.front( ) );
			pending_.pop_front( );

			const auto posted = post_one( frame );

			if ( !posted ) {
				report_end( posted.error( ).msg );

				return;
			}

			if ( std::chrono::steady_clock::now( ) >= deadline ) {
				return;
			}
		}
	}

	auto http_transport::run( ) -> status {
		while ( alive_ ) {
			pump( std::chrono::milliseconds{ 50 } );

			if ( !alive_ ) {
				break;
			}

			// No listening stream: nothing arrives unsent, so an idle transport must not spin.
			if ( pending_.empty( ) ) {
				std::this_thread::sleep_for( std::chrono::milliseconds{ 10 } );
			}
		}

		return { };
	}

	auto http_transport::close_input( ) -> status {
		return { };
	}

	auto http_transport::wait_exit( const std::chrono::milliseconds timeout ) -> void {
		(void)timeout;
	}

	auto http_transport::stop( ) -> void {
		alive_ = false;
		pending_.clear( );
	}

	auto http_transport::alive( ) -> bool {
		return alive_;
	}

	auto http_transport::stderr_text( ) const -> std::string {
		return last_error_;
	}

	auto http_transport::finalize( ) -> void { }

	auto http_transport::report_end( const std::string& detail ) -> void {
		if ( !alive_ ) {
			return;
		}

		alive_ = false;
		last_error_ = detail;

		if ( on_eof ) {
			on_eof( );
		}
	}

}
