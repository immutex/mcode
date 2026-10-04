#include "mcode/ext/api_gate.hxx"

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/net/http_client.hxx"

namespace mcode::ext {

	namespace {

		// the manifest is author-written; a host comparison is case-insensitive.
		[[nodiscard]] auto folded( const std::string_view text ) -> std::string {
			auto out = std::string{ text };

			for ( auto& character : out ) {
				if ( character >= 'A' && character <= 'Z' ) {
					character = static_cast< char >( character - 'A' + 'a' );
				}
			}

			return out;
		}

	}

	auto deny_permission( lua_State* state, const char* permission, const char* entry )
		-> int {
		lua_pushnil( state );

		lua_pushliteral( state, "permission denied: '" );
		lua_pushstring( state, permission );
		lua_pushliteral( state, "' is required for " );
		lua_pushstring( state, entry );

		lua_concat( state, 4 );

		return 2;
	}

	auto manifest_allows( const api_surface& surface, const char* permission ) -> bool {
		return surface.manifest( ).has_permission( permission );
	}

	auto url_host( const std::string_view url ) -> std::string {
		const auto parsed = mcode::net::parse_url( url );

		if ( !parsed ) {
			return { };
		}

		return folded( parsed->host );
	}

	auto host_declared( const api_surface& surface, const std::string_view host ) -> bool {
		if ( host.empty( ) ) {
			return false;
		}

		const auto wanted = folded( host );

		for ( const auto declared : surface.manifest( ).net_hosts( ) ) {
			if ( folded( declared ) == wanted ) {
				return true;
			}
		}

		return false;
	}

	auto credential_declared( const api_surface& surface, const std::string_view name )
		-> bool {
		if ( name.empty( ) ) {
			return false;
		}

		for ( const auto declared : surface.manifest( ).credential_names( ) ) {
			if ( declared == name ) {
				return true;
			}
		}

		return false;
	}

}
