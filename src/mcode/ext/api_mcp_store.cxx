#include "mcode/ext/api_mcp.hxx"

#include <algorithm>
#include <utility>

namespace mcode::ext {

	auto mcp_server_store::add( mcode::mcp::server_config server ) -> status {
		if ( find( server.name ) != nullptr ) {
			return std::unexpected( fail( errc::config,
				"an MCP server named '" + server.name + "' is already declared" ) );
		}

		servers_.push_back( std::move( server ) );

		return { };
	}

	auto mcp_server_store::find( const std::string_view name ) const
		-> const mcode::mcp::server_config* {
		const auto found = std::find_if( servers_.begin( ), servers_.end( ),
			[ & ]( const mcode::mcp::server_config& server ) {
				return server.name == name;
			} );

		return found != servers_.end( ) ? &*found : nullptr;
	}

}
