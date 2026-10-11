#include "mcode/cli/egress_setup.hxx"

#include <cstdio>
#include <string>
#include <vector>

#include "mcode/net/egress_policy.hxx"

namespace mcode::cli {

	namespace {

		// `[egress] allow = ["github.com", "*.githubusercontent.com"]`.
		//
		// Deliberately not `enabled = true` plus a list: an empty allowlist is no
		// network, so the presence of the list IS the switch and there is no state
		// where the proxy runs and grants nothing.
		auto rules_from( const config::merged_config& values )
			-> std::vector< net::egress_rule > {
			const auto entries = values.get_string_array( "egress.allow" );

			if ( !entries ) {
				return { };
			}

			auto rules = std::vector< net::egress_rule >{ };

			for ( const auto& entry : *entries ) {
				auto rule = net::egress_rule{ };
				rule.host = entry;
				rule.allow = true;
				rule.port = 443;

				// A host may name its port: `example.com:8443`.
				const auto colon = entry.rfind( ':' );

				if ( colon != std::string::npos && colon > 0 ) {
					auto port = 0;

					try {
						port = std::stoi( entry.substr( colon + 1 ) );
					} catch ( ... ) {
						port = 0;
					}

					if ( port > 0 && port <= 65535 ) {
						rule.host = entry.substr( 0, colon );
						rule.port = static_cast< std::uint16_t >( port );
					}
				}

				rules.push_back( std::move( rule ) );
			}

			return rules;
		}

	}

	auto start_egress_if_configured( const config::merged_config& values )
		-> std::unique_ptr< net::egress_proxy > {
		auto rules = rules_from( values );

		if ( rules.empty( ) ) {
			return nullptr;
		}

		auto started = net::egress_proxy::start( net::egress_policy::from_rules( std::move( rules ) ) );

		if ( !started ) {
			std::fprintf( stderr, "mcode: egress proxy did not start: %s\n",
				started.error( ).msg.c_str( ) );

			return nullptr;
		}

		auto proxy = std::make_unique< net::egress_proxy >( std::move( *started ) );

		proxy->serve( );

		return proxy;
	}

}
