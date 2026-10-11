#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include "mcode/net/egress_policy.hxx"
#include "mcode/net/egress_proxy.hxx"

using namespace mcode;

namespace {

	namespace asio = boost::asio;
	using tcp = asio::ip::tcp;

	// Sends one raw HTTP request to the proxy and returns what comes back. The
	// proxy answers a refused host with a status line before any relay, so this
	// needs no upstream server.
	auto request_through_proxy( const std::uint16_t port, const std::string& request )
		-> std::string {
		auto context = asio::io_context{ };
		auto socket = tcp::socket{ context };
		auto error = boost::system::error_code{ };

		socket.connect( tcp::endpoint{ asio::ip::make_address( "127.0.0.1" ), port }, error );

		if ( error ) {
			return { };
		}

		asio::write( socket, asio::buffer( request ), error );

		auto buffer = std::vector< char >( 512 );
		const auto read = socket.read_some( asio::buffer( buffer ), error );

		if ( error && read == 0 ) {
			return { };
		}

		return std::string{ buffer.data( ), read };
	}

}

TEST_CASE( "the proxy refuses a host that is not on the allowlist", "[egress][proxy]" ) {
	auto started = net::egress_proxy::start(
		net::egress_policy::from_rules( { { .host = "allowed.example", .port = 443 } } ) );

	REQUIRE( started.has_value( ) );

	started->serve( );

	// Port 80 so the CONNECT port check does not decide this before the host does.
	const auto response = request_through_proxy( started->http_port( ),
		"GET http://denied.example:80/x HTTP/1.1\r\nHost: denied.example\r\n\r\n" );

	started->stop( );

	REQUIRE( response.find( "403" ) != std::string::npos );
	REQUIRE( started->denied_count( ) >= 1 );
	REQUIRE( started->allowed_count( ) == 0 );
}

TEST_CASE( "the proxy refuses a CONNECT to a reserved port", "[egress][proxy]" ) {
	auto started = net::egress_proxy::start( net::egress_policy::from_rules(
		{ { .host = "allowed.example", .port = 25 } } ) );

	REQUIRE( started.has_value( ) );

	started->serve( );

	// On the allowlist and on an allowlisted port, so only RFC 9110's reserved-port
	// rule can refuse it.
	const auto response = request_through_proxy( started->http_port( ),
		"CONNECT allowed.example:25 HTTP/1.1\r\n\r\n" );

	started->stop( );

	REQUIRE( response.find( "403" ) != std::string::npos );
}

TEST_CASE( "the proxy refuses a request it cannot parse", "[egress][proxy]" ) {
	auto started = net::egress_proxy::start( net::egress_policy::deny_all( ) );

	REQUIRE( started.has_value( ) );

	started->serve( );

	const auto response = request_through_proxy( started->http_port( ), "not a request\r\n\r\n" );

	started->stop( );

	REQUIRE( response.find( "400" ) != std::string::npos );
}

TEST_CASE( "the proxy advertises itself through the environment it returns", "[egress][proxy]" ) {
	auto started = net::egress_proxy::start( net::egress_policy::deny_all( ) );

	REQUIRE( started.has_value( ) );
	REQUIRE( started->http_port( ) != 0 );
	REQUIRE( started->socks_port( ) != 0 );
	REQUIRE( started->http_port( ) != started->socks_port( ) );

	const auto environment = started->environment( );

	REQUIRE( environment.find( "HTTP_PROXY=" ) != std::string::npos );
	REQUIRE( environment.find( "HTTPS_PROXY=" ) != std::string::npos );
	REQUIRE( environment.find( "ALL_PROXY=socks5://" ) != std::string::npos );

	// Both listeners are on loopback, so nothing outside this machine can reach
	// them and no configuration can widen that.
	REQUIRE( environment.find( "127.0.0.1" ) != std::string::npos );
}

TEST_CASE( "a moved-from proxy is inert rather than dangling", "[egress][proxy]" ) {
	auto started = net::egress_proxy::start( net::egress_policy::deny_all( ) );

	REQUIRE( started.has_value( ) );

	auto moved = std::move( *started );

	REQUIRE( moved.http_port( ) != 0 );

	// The move left the source empty; calling through it must be a no-op, not a
	// read of a freed implementation.
	REQUIRE( started->http_port( ) == 0 );
	REQUIRE( started->denied_count( ) == 0 );

	moved.stop( );
}
