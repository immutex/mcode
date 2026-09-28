#include "mcode/net/http_client.hxx"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>

namespace mcode::net {

	namespace {

		namespace beast = boost::beast;
		namespace http = beast::http;
		namespace asio = boost::asio;

		using tcp = asio::ip::tcp;

		[[nodiscard]] auto classify( const boost::system::error_code& error_code ) -> errc {
			if ( !error_code ) {
				return errc::ok;
			}

			switch ( error_code.value( ) ) {
				case asio::error::timed_out:
				case asio::error::operation_aborted:
					return errc::cancelled;

				case asio::error::connection_refused:
				case asio::error::host_not_found:
				case asio::error::host_unreachable:
					return errc::io;

				default:
					return error_code.category( ) == asio::error::get_netdb_category( ) ? errc::io
																						: errc::protocol;
			}
		}

		[[nodiscard]] auto lower( const std::string_view text ) -> std::string {
			auto out = std::string{ text };

			std::transform( out.begin( ), out.end( ), out.begin( ),
				[]( const unsigned char byte ) { return static_cast< char >( std::tolower( byte ) ); } );

			return out;
		}

		[[nodiscard]] auto is_https( const std::string_view scheme ) -> bool { return scheme == "https"; }

	}

	auto parse_url( const std::string_view text ) -> result< url > {
		const auto scheme_end = text.find( "://" );

		if ( scheme_end == std::string_view::npos ) {
			return std::unexpected( fail( errc::protocol, "URL has no scheme: " + std::string{ text } ) );
		}

		auto out = url{ };
		out.scheme = lower( text.substr( 0, scheme_end ) );

		if ( out.scheme != "http" && out.scheme != "https" ) {
			return std::unexpected( fail( errc::unsupported, "unsupported scheme: " + out.scheme ) );
		}

		const auto rest = text.substr( scheme_end + 3 );

		auto authority = rest;
		auto path = std::string_view{ };
		const auto slash = rest.find( '/' );

		if ( slash != std::string_view::npos ) {
			authority = rest.substr( 0, slash );
			path = rest.substr( slash );
		}

		if ( authority.find( '@' ) != std::string_view::npos ) {
			return std::unexpected( fail( errc::protocol,
				"URL contains userinfo, which is not accepted: " + std::string{ authority } ) );
		}

		const auto colon = authority.rfind( ':' );

		if ( colon != std::string_view::npos && authority.find( ']' ) == std::string_view::npos ) {
			out.host = lower( authority.substr( 0, colon ) );
			out.port = std::string{ authority.substr( colon + 1 ) };
		} else {
			out.host = lower( authority );
		}

		if ( out.host.empty( ) ) {
			return std::unexpected( fail( errc::protocol, "URL has an empty host" ) );
		}

		if ( out.host.back( ) == '.' ) {
			return std::unexpected( fail( errc::protocol,
				"URL host has a trailing dot, which is not accepted: " + out.host ) );
		}

		if ( out.port.empty( ) ) {
			out.port = is_https( out.scheme ) ? "443" : "80";
		}

		out.target = path.empty( ) ? "/" : std::string{ path };

		return out;
	}

	auto http_client::send( const http_request& request ) -> result< http_response > {
		auto parsed = parse_url( request.url );

		if ( !parsed ) {
			return std::unexpected( parsed.error( ) );
		}

		if ( is_https( parsed->scheme ) ) {
			return std::unexpected( fail( errc::unsupported, "TLS is not linked in this build" ) );
		}

		try {
			auto context = asio::io_context{ };
			auto resolver = tcp::resolver{ context };
			auto stream = beast::tcp_stream{ context };

			const auto endpoints = resolver.resolve( parsed->host, parsed->port );
			stream.expires_after( std::chrono::seconds( request.timeout_seconds ) );
			stream.connect( endpoints );

			auto message = http::request< http::string_body >{
				http::string_to_verb( request.method ), parsed->target, 11 };

			message.set( http::field::host, parsed->host );
			message.set( http::field::user_agent, "mcode" );

			for ( const auto& [ key, value ] : request.headers ) {
				message.set( key, value );
			}

			if ( !request.body.empty( ) ) {
				message.body( ) = request.body;
				message.prepare_payload( );
			}

			http::write( stream, message );

			auto buffer = beast::flat_buffer{ };
			auto parser = http::response_parser< http::string_body >{ };
			parser.body_limit( max_response_bytes_ );
			http::read( stream, buffer, parser );

			auto ignored = boost::system::error_code{ };
			stream.socket( ).shutdown( tcp::socket::shutdown_both, ignored );

			auto out = http_response{ };
			out.status = static_cast< int >( parser.get( ).result_int( ) );
			out.body = parser.get( ).body( );

			for ( const auto& field : parser.get( ).base( ) ) {
				out.headers.emplace( std::string{ field.name_string( ) }, std::string{ field.value( ) } );
			}

			return out;
		} catch ( const boost::system::system_error& exception ) {
			return std::unexpected(
				fail( classify( exception.code( ) ), std::string{ "http: " } + exception.what( ) ) );
		}
	}

	auto http_client::stream_sse( const http_request& request, sse_parser::event_callback on_event,
		http_failure* failure ) -> status {
		auto parsed = parse_url( request.url );

		if ( !parsed ) {
			return std::unexpected( parsed.error( ) );
		}

		if ( is_https( parsed->scheme ) ) {
			return std::unexpected( fail( errc::unsupported, "TLS is not linked in this build" ) );
		}

		try {
			auto context = asio::io_context{ };
			auto resolver = tcp::resolver{ context };
			auto stream = beast::tcp_stream{ context };

			const auto endpoints = resolver.resolve( parsed->host, parsed->port );
			stream.expires_after( std::chrono::seconds( request.timeout_seconds ) );
			stream.connect( endpoints );

			auto message = http::request< http::string_body >{
				http::string_to_verb( request.method ), parsed->target, 11 };

			message.set( http::field::host, parsed->host );
			message.set( http::field::accept, "text/event-stream" );
			message.set( http::field::cache_control, "no-cache" );
			message.set( http::field::user_agent, "mcode" );

			for ( const auto& [ key, value ] : request.headers ) {
				message.set( key, value );
			}

			if ( !request.body.empty( ) ) {
				message.body( ) = request.body;
				message.prepare_payload( );
			}

			http::write( stream, message );

			auto buffer = beast::flat_buffer{ };
			auto parser = http::response_parser< http::string_body >{ };
			parser.body_limit( MAX_ERROR_BODY_BYTES );
			http::read_header( stream, buffer, parser );

			const auto status_code = static_cast< int >( parser.get( ).result_int( ) );

			if ( parser.get( ).result( ) != http::status::ok ) {
				// The body is what distinguishes a retryable 429 from a billing one,
				// so it is read rather than skipped. Best-effort: a truncated or
				// unreadable body must not cost us the status we already have.
				auto body_error = boost::system::error_code{ };
				http::read( stream, buffer, parser, body_error );

				if ( failure != nullptr ) {
					failure->status = status_code;
					failure->body = parser.get( ).body( );

					for ( const auto& field : parser.get( ).base( ) ) {
						failure->headers.emplace( std::string{ field.name_string( ) },
							std::string{ field.value( ) } );
					}
				}

				return std::unexpected( fail( errc::protocol,
					"SSE request returned HTTP " + std::to_string( status_code ) ) );
			}

			auto parser_state = sse_parser{ std::move( on_event ) };
			auto total = std::uint64_t{ 0 };

			// `read_header` leaves whatever it read past the header in `buffer`, and
			// a server that writes its first SSE event in the same segment as the
			// response headers puts it there. Reading straight from the socket would
			// drop those bytes, so the buffer is drained first.
			if ( buffer.size( ) > 0 ) {
				const auto buffered = static_cast< const char* >( buffer.data( ).data( ) );

				total += buffer.size( );
				parser_state.feed( std::string_view{ buffered, buffer.size( ) } );
				buffer.consume( buffer.size( ) );
			}

			auto chunk = std::array< char, 8192 >{ };

			while ( true ) {
				const auto read = stream.read_some( asio::buffer( chunk ) );

				if ( read == 0 ) {
					break;
				}

				total += read;

				if ( total > max_response_bytes_ ) {
					return std::unexpected(
						fail( errc::protocol, "SSE stream exceeded the response cap" ) );
				}

				parser_state.feed( std::string_view{ chunk.data( ), read } );
			}

			parser_state.finish( );

			return { };
		} catch ( const boost::system::system_error& exception ) {
			if ( exception.code( ) == http::error::end_of_stream || exception.code( ) == asio::error::eof ) {
				return { };
			}

			return std::unexpected(
				fail( classify( exception.code( ) ), std::string{ "sse: " } + exception.what( ) ) );
		}
	}

}
