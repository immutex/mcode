#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/net/egress_policy.hxx"

using namespace mcode;

namespace {

	auto rule( std::string host, const std::uint16_t port = 443, const bool allow = true ) {
		return net::egress_rule{ .host = std::move( host ), .port = port, .allow = allow };
	}

}

TEST_CASE( "an empty policy denies everything", "[egress]" ) {
	const auto policy = net::egress_policy::deny_all( );

	// The safe state has to be the one you get by not deciding: a policy that
	// allowed by default would make every configuration mistake a network grant.
	REQUIRE( policy.decide( "example.com", 443 ) == net::egress_decision::deny );
	REQUIRE( policy.decide( "localhost", 80 ) == net::egress_decision::deny );
	REQUIRE( policy.decide( "", 443 ) == net::egress_decision::deny );
}

TEST_CASE( "an allowlist admits only what it names", "[egress]" ) {
	const auto policy = net::egress_policy::from_rules( { rule( "github.com" ) } );

	REQUIRE( policy.decide( "github.com", 443 ) == net::egress_decision::allow );
	REQUIRE( policy.decide( "GitHub.com", 443 ) == net::egress_decision::allow );
	REQUIRE( policy.decide( "evil.com", 443 ) == net::egress_decision::deny );
	REQUIRE( policy.decide( "github.com.evil.com", 443 ) == net::egress_decision::deny );
}

TEST_CASE( "a wildcard matches subdomains but not the apex or a suffix twin", "[egress]" ) {
	const auto policy = net::egress_policy::from_rules( { rule( "*.github.com" ) } );

	REQUIRE( policy.decide( "api.github.com", 443 ) == net::egress_decision::allow );
	REQUIRE( policy.decide( "raw.github.com", 443 ) == net::egress_decision::allow );

	// Neither of these is a subdomain, and a naive suffix test would admit both.
	REQUIRE( policy.decide( "github.com", 443 ) == net::egress_decision::deny );
	REQUIRE( policy.decide( "notgithub.com", 443 ) == net::egress_decision::deny );
}

TEST_CASE( "a bare wildcard is not a rule", "[egress]" ) {
	const auto policy = net::egress_policy::from_rules( { rule( "*" ) } );

	// Allowlisting everything is the absence of a policy; writing it down as one
	// makes it look like a decision.
	REQUIRE( policy.decide( "anything.example", 443 ) == net::egress_decision::deny );
	REQUIRE( policy.rules( ).empty( ) );
}

TEST_CASE( "deny beats allow for the same host", "[egress]" ) {
	const auto policy = net::egress_policy::from_rules( {
		rule( "*.example.com" ),
		rule( "secrets.example.com", 443, false ),
	} );

	REQUIRE( policy.decide( "api.example.com", 443 ) == net::egress_decision::allow );
	REQUIRE( policy.decide( "secrets.example.com", 443 ) == net::egress_decision::deny );
}

TEST_CASE( "the port is part of the match", "[egress]" ) {
	const auto policy = net::egress_policy::from_rules( { rule( "example.com", 443 ) } );

	REQUIRE( policy.decide( "example.com", 443 ) == net::egress_decision::allow );
	REQUIRE( policy.decide( "example.com", 22 ) == net::egress_decision::deny );
}

TEST_CASE( "loopback and metadata addresses are always denied", "[egress]" ) {
	// Independent of the host list: this is what stops an allowlisted name from
	// being aimed at a LAN service or a cloud credential endpoint.
	REQUIRE( net::egress_policy::address_is_denied( "127.0.0.1" ) );
	REQUIRE( net::egress_policy::address_is_denied( "127.1.2.3" ) );
	REQUIRE( net::egress_policy::address_is_denied( "169.254.169.254" ) );
	REQUIRE( net::egress_policy::address_is_denied( "100.100.100.200" ) );
	REQUIRE( net::egress_policy::address_is_denied( "::1" ) );
	REQUIRE( net::egress_policy::address_is_denied( "::ffff:127.0.0.1" ) );
	REQUIRE( net::egress_policy::address_is_denied( "fe80::1" ) );

	REQUIRE( !net::egress_policy::address_is_denied( "140.82.121.4" ) );
	REQUIRE( !net::egress_policy::address_is_denied( "2606:50c0:8000::153" ) );
}

TEST_CASE( "CONNECT is restricted to web-ish ports", "[egress]" ) {
	REQUIRE( net::connect_port_allowed( 443 ) );
	REQUIRE( net::connect_port_allowed( 22 ) );

	// RFC 9110 warns explicitly about tunnels to reserved ports, so an allowlisted
	// host is not enough on its own.
	REQUIRE( !net::connect_port_allowed( 25 ) );
	REQUIRE( !net::connect_port_allowed( 445 ) );
	REQUIRE( !net::connect_port_allowed( 3306 ) );
}
