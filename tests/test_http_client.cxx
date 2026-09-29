#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "mcode/net/http_client.hxx"
#include "mcode/net/sse.hxx"

#include "socket_test_helpers.hxx"

using mcode::net::http_client;
using mcode::net::http_failure;
using mcode::net::http_request;
using mcode::net::sse_event;

namespace {

	using mcode::test::INVALID_SOCKET_HANDLE;
	using mcode::test::close_socket;
	using mcode::test::host_to_network_long;
	using mcode::test::network_to_host_short;
	using mcode::test::receive_bytes;
	using mcode::test::send_bytes;
	using mcode::test::socket_handle;

	// A one-shot HTTP server on an ephemeral loopback port.
	//
	// The SSE path cannot be tested without a real socket: framing, the buffer
	// left over from the header read, and the failure status are all properties of
	// the transport, and a mocked transport would assert the mock rather than the
	// client. No external network is touched.
	class loopback_server {
	public:
		loopback_server( const std::string& response, const bool close_immediately = false )
			: response_( response ), close_immediately_( close_immediately ) {
		#if defined( _WIN32 )
			auto data = WSADATA{ };
			::WSAStartup( MAKEWORD( 2, 2 ), &data );
		#endif

			socket_ = ::socket( AF_INET, SOCK_STREAM, 0 );

			auto address = sockaddr_in{ };
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = host_to_network_long( INADDR_LOOPBACK );
			address.sin_port = 0;

			// A failure here is not recoverable and the test would hang on connect,
			// so the port stays zero and `url` produces a refused connection.
			const auto bound = ::bind( socket_, reinterpret_cast< sockaddr* >( &address ),
				sizeof( address ) ) == 0;

			if ( !bound || ::listen( socket_, 1 ) != 0 ) {
				return;
			}

			auto length = static_cast< socklen_t >( sizeof( address ) );

			if ( ::getsockname( socket_, reinterpret_cast< sockaddr* >( &address ), &length ) != 0 ) {
				return;
			}

			port_ = network_to_host_short( address.sin_port );

			worker_ = std::thread{ [this] { serve( ); } };
		}

		~loopback_server( ) {
			if ( worker_.joinable( ) ) {
				worker_.join( );
			}

			close_socket( socket_ );
		}

		loopback_server( const loopback_server& ) = delete;
		auto operator=( const loopback_server& ) -> loopback_server& = delete;

		[[nodiscard]] auto url( ) const -> std::string {
			return "http://127.0.0.1:" + std::to_string( port_ ) + "/v1/chat";
		}

	private:
		static auto close_socket( const socket_handle handle ) -> void {
		#if defined( _WIN32 )
			::closesocket( handle );
		#else
			::close( handle );
		#endif
	}

		auto serve( ) -> void {
			const auto accepted = ::accept( socket_, nullptr, nullptr );

			if ( accepted == INVALID_SOCKET_HANDLE ) {
				return;
			}

			// Drain the request so the client's write completes. A single read is
			// enough for a body this size.
			auto scratch = std::array< char, 4096 >{ };
			receive_bytes( accepted, scratch );

			if ( !response_.empty( ) ) {
				(void)send_bytes( accepted, response_ );
			}

			if ( close_immediately_ ) {
				close_socket( accepted );

				return;
			}

			// Let the peer read what was sent before the socket goes away.
			std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
			close_socket( accepted );
		}

		std::string response_;
		bool close_immediately_ = false;
		socket_handle socket_ = INVALID_SOCKET_HANDLE;
		std::uint16_t port_ = 0;
		std::thread worker_;
	};

}

TEST_CASE( "a failed SSE response reports its status, headers and body", "[http]" ) {
	// The whole reason the failure struct exists: whether a 429 is retryable lives
	// in the body, and the delay lives in a header. A status-only error makes both
	// undecidable.
	const auto body = std::string{ R"({"error":{"type":"insufficient_quota"}})" };
	const auto response = std::string{ "HTTP/1.1 429 Too Many Requests\r\n"
		"Content-Type: application/json\r\n"
		"Retry-After: 7\r\n"
		"Content-Length: " } + std::to_string( body.size( ) ) + "\r\n"
		"Connection: close\r\n\r\n" + body;

	const auto server = loopback_server{ response };

	auto request = http_request{ };
	request.url = server.url( );

	auto failure = http_failure{ };
	auto received = std::vector< sse_event >{ };

	auto result = http_client{ }.stream_sse( request,
		[&received]( sse_event&& event ) { received.push_back( std::move( event ) ); }, &failure );

	REQUIRE_FALSE( result );
	CHECK( received.empty( ) );

	CHECK( failure.status == 429 );
	CHECK( failure.body.find( "insufficient_quota" ) != std::string::npos );

	// The header is what drives the backoff delay.
	auto retry_after = failure.headers.find( "Retry-After" );

	if ( retry_after == failure.headers.end( ) ) {
		retry_after = failure.headers.find( "retry-after" );
	}

	REQUIRE( retry_after != failure.headers.end( ) );
	CHECK( retry_after->second == "7" );
}

TEST_CASE( "an SSE event arriving with the headers is not dropped", "[http]" ) {
	// `read_header` can leave body bytes in its buffer, and a server that writes the
	// first event in the same segment as the response headers puts them there.
	// Reading straight from the socket after the header would lose them.
	const auto response = std::string{ "HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Connection: close\r\n\r\n" } + "data: {\"delta\":\"first\"}\n\ndata: [DONE]\n\n";

	const auto server = loopback_server{ response };

	auto request = http_request{ };
	request.url = server.url( );

	auto received = std::vector< sse_event >{ };

	auto result = http_client{ }.stream_sse( request,
		[&received]( sse_event&& event ) { received.push_back( std::move( event ) ); } );

	REQUIRE( static_cast< bool >( result ) );

	// Both events, including the one that shared the header's segment.
	REQUIRE( received.size( ) == 2 );
	CHECK( received[ 0 ].data.find( "first" ) != std::string::npos );
	CHECK( received[ 1 ].data == "[DONE]" );
}

TEST_CASE( "a throwing event callback becomes a failure, not a terminate", "[http]" ) {
	// The model client reports an unusable stream event by throwing out of the
	// callback. Nothing between it and the transport catches, so a throw that
	// escaped here reached std::terminate: observed as 0xC0000409 against a real
	// gateway, on whichever request first delivered an event the applier rejected.
	// The boundary must convert it into a failure.
	const auto response = std::string{ "HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Connection: close\r\n\r\n" } + "data: {\"delta\":\"first\"}\n\n";

	const auto server = loopback_server{ response };

	auto request = http_request{ };
	request.url = server.url( );

	auto result = http_client{ }.stream_sse( request,
		[]( sse_event&& ) -> void { throw std::runtime_error{ "malformed event" }; } );

	REQUIRE_FALSE( static_cast< bool >( result ) );
	CHECK( result.error( ).code == mcode::errc::protocol );
}

TEST_CASE( "a chunked SSE body is decoded before it is parsed", "[http]" ) {
	// Cloudflare fronts the gateway and sends the stream chunked. The body is read
	// straight off the socket, so the framing reached the event parser: a chunk
	// boundary inside an event split its JSON across two lines, the continuation
	// was discarded as a line with no colon, and the truncated payload was
	// rejected. Observed against the real endpoint as 0xC0000409; a de-chunking
	// loopback proxy hid it.
	const auto first = std::string{ "data: {\"del" };
	const auto second = std::string{ "ta\":\"split\"}\n\n" };

	const auto size = []( const std::size_t bytes ) {
		auto out = std::string{ };
		auto value = bytes;

		do {
			const auto digit = value & 0xf;
			out.insert( out.begin( ),
				static_cast< char >( digit < 10 ? '0' + digit : 'a' + digit - 10 ) );
			value >>= 4;
		} while ( value != 0 );

		return out;
	};

	const auto response = std::string{ "HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: close\r\n\r\n" }
		+ size( first.size( ) ) + "\r\n" + first + "\r\n"
		+ size( second.size( ) ) + "\r\n" + second + "\r\n"
		+ "0\r\n\r\n";

	const auto server = loopback_server{ response };

	auto request = http_request{ };
	request.url = server.url( );

	auto received = std::vector< sse_event >{ };

	auto result = http_client{ }.stream_sse( request,
		[&received]( sse_event&& event ) { received.push_back( std::move( event ) ); } );

	REQUIRE( static_cast< bool >( result ) );
	REQUIRE( received.size( ) == 1 );
	CHECK( received[ 0 ].data == "{\"delta\":\"split\"}" );
}

TEST_CASE( "a successful stream leaves the failure struct untouched", "[http]" ) {
	const auto response = std::string{ "HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Connection: close\r\n\r\n" } + "data: hi\n\n";

	const auto server = loopback_server{ response };

	auto request = http_request{ };
	request.url = server.url( );

	auto failure = http_failure{ };
	failure.status = -1;

	auto result = http_client{ }.stream_sse( request, []( sse_event&& ) { }, &failure );

	REQUIRE( static_cast< bool >( result ) );
	CHECK( failure.status == -1 );
	CHECK( failure.body.empty( ) );
}

TEST_CASE( "https is not refused", "[http][tls]" ) {
	// T1's acceptance: the https path is wired, not stubbed. The connection to
	// the loopback TLS-less port fails as an IO error, not as `unsupported` --
	// `unsupported` here would mean TLS is not linked, which is the regression
	// this test exists to catch. No external network: the host is loopback.
	auto request = http_request{ };
	request.url = "https://127.0.0.1:1/v1/chat";
	request.timeout_seconds = 1;

	const auto result = http_client{ }.stream_sse( request, []( sse_event&& ) { } );

	REQUIRE_FALSE( static_cast< bool >( result ) );
	CHECK( result.error( ).code != mcode::errc::unsupported );

	const auto sent = http_client{ }.send( request );

	REQUIRE_FALSE( static_cast< bool >( sent ) );
	CHECK( sent.error( ).code != mcode::errc::unsupported );
}
