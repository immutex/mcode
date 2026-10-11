#include "mcode/net/egress_policy.hxx"

#include <array>
#include <charconv>

#include "mcode/support/text.hxx"

namespace mcode::net {

	namespace {

		// RFC 9110 §9.3.6: a tunnel to a well-known or reserved port is a risk the
		// proxy should not accept just because a host was allowlisted.
		constexpr std::array< std::uint16_t, 6 > CONNECT_PORTS = {
			443, 8443, 4443, 9443, 22, 9418,
		};

		auto host_matches( const std::string_view pattern, const std::string_view host ) -> bool {
			if ( pattern == host ) {
				return true;
			}

			if ( !pattern.starts_with( "*." ) ) {
				return false;
			}

			const auto suffix = pattern.substr( 1 );

			// `*.example.com` must not match `example.com` itself, and must not match
			// `notexample.com`: the dot in the suffix is what enforces both.
			if ( host.size( ) <= suffix.size( ) ) {
				return false;
			}

			return text::ascii_lower( host ).ends_with( text::ascii_lower( suffix ) );
		}

		auto parse_ipv4( const std::string_view literal, std::array< std::uint8_t, 4 >& out )
			-> bool {
			auto part = std::size_t{ 0 };
			auto start = std::size_t{ 0 };

			for ( auto index = std::size_t{ 0 }; index <= literal.size( ); ++index ) {
				if ( index == literal.size( ) || literal[ index ] == '.' ) {
					if ( part >= 4 || index == start ) {
						return false;
					}

					auto value = 0;

					const auto parsed = std::from_chars( literal.data( ) + start,
						literal.data( ) + index, value );

					if ( parsed.ec != std::errc{ } || value > 255 ) {
						return false;
					}

					out[ part ] = static_cast< std::uint8_t >( value );
					++part;
					start = index + 1;
				}
			}

			return part == 4;
		}

	}

	auto connect_port_allowed( const std::uint16_t port ) noexcept -> bool {
		for ( const auto allowed : CONNECT_PORTS ) {
			if ( allowed == port ) {
				return true;
			}
		}

		return false;
	}

	auto egress_policy::deny_all( ) -> egress_policy {
		return egress_policy{ };
	}

	auto egress_policy::from_rules( std::vector< egress_rule > rules ) -> egress_policy {
		auto policy = egress_policy{ };

		for ( auto& rule : rules ) {
			if ( rule.host.empty( ) || rule.host == "*" ) {
				continue;
			}

			rule.host = text::ascii_lower( rule.host );
			policy.rules_.push_back( std::move( rule ) );
		}

		return policy;
	}

	auto egress_policy::decide( const std::string_view host, const std::uint16_t port ) const
		-> egress_decision {
		if ( host.empty( ) ) {
			return egress_decision::deny;
		}

		const auto folded = text::ascii_lower( host );

		// Deny beats allow, so a broad grant can be carved back down.
		for ( const auto& rule : rules_ ) {
			if ( !rule.allow && rule.port == port && host_matches( rule.host, folded ) ) {
				return egress_decision::deny;
			}
		}

		for ( const auto& rule : rules_ ) {
			if ( rule.allow && rule.port == port && host_matches( rule.host, folded ) ) {
				return egress_decision::allow;
			}
		}

		return egress_decision::deny;
	}

	auto egress_policy::address_is_denied( const std::string_view literal ) -> bool {
		auto octets = std::array< std::uint8_t, 4 >{ };

		if ( parse_ipv4( literal, octets ) ) {
			if ( octets[ 0 ] == 127 ) {
				return true;
			}

			if ( octets[ 0 ] == 169 && octets[ 1 ] == 254 ) {
				return true;
			}

			if ( octets[ 0 ] == 0 || octets[ 0 ] >= 224 ) {
				return true;
			}

			// The metadata service used by the major clouds. Reaching it from a
			// sandboxed child is credential theft with extra steps.
			if ( octets[ 0 ] == 100 && octets[ 1 ] == 100 && octets[ 2 ] == 100
				&& octets[ 3 ] == 200 ) {
				return true;
			}

			if ( octets[ 0 ] == 168 && octets[ 1 ] == 63 && octets[ 2 ] == 129
				&& octets[ 3 ] == 16 ) {
				return true;
			}

			return false;
		}

		const auto folded = text::ascii_lower( literal );

		if ( folded == "::1" || folded == "::" || folded == "0:0:0:0:0:0:0:1" ) {
			return true;
		}

		// IPv4-mapped loopback, which macOS does not match against "localhost".
		if ( folded == "::ffff:127.0.0.1" ) {
			return true;
		}

		if ( folded.starts_with( "fe80:" ) || folded.starts_with( "ff" ) ) {
			return true;
		}

		return false;
	}

}
