#include "mcode/ext/api_net.hxx"

#include <string>
#include <utility>

#include "lua.h"

#include "mcode/ext/api.hxx"
#include "mcode/ext/api_gate.hxx"
#include "mcode/ext/api_internal.hxx"
#include "mcode/ext/lua_json.hxx"
#include "mcode/net/http_client.hxx"

namespace mcode::ext {

	namespace {

		// The bound table: connect + total timeout, and a response byte cap.
		// Every entry states a bound; these are the network rows.
		inline constexpr auto NET_TIMEOUT_SECONDS = std::int64_t{ 30 };
		inline constexpr auto NET_MAX_RESPONSE_BYTES = std::uint64_t{ 4 * 1024 * 1024 };

		// The host an URL is addressed to, lowercased. The allowlist compares
		// hosts, not full URLs: a path or query is not a different destination.
		[[nodiscard]] auto host_of( const std::string_view url ) -> std::string {
			const auto parsed = mcode::net::parse_url( url );

			if ( !parsed ) {
				return { };
			}

			auto host = parsed->host;

			for ( auto& character : host ) {
				if ( character >= 'A' && character <= 'Z' ) {
					character = static_cast< char >( character - 'A' + 'a' );
				}
			}

			return host;
		}

		// The hosts the calling extension's manifest declares. The manifest's
		// `net_hosts` key is the allowlist; an absent key with the `net`
		// permission permits nothing, which is fail-closed.
		[[nodiscard]] auto declared_hosts( const api_surface& self )
			-> const std::vector< std::string >& {
			return self.net_hosts( );
		}

		// The one policy check. Both entry points go through it, so there is
		// exactly one place a host rule can drift.
		[[nodiscard]] auto host_allowed( const api_surface& self,
			const std::string_view url, std::string& reason ) -> bool {
			if ( !manifest_allows( self, "net" ) ) {
				reason = "permission denied: 'net' is required for mcode.net";

				return false;
			}

			const auto host = host_of( url );

			if ( host.empty( ) ) {
				reason = "the URL could not be parsed";

				return false;
			}

			for ( const auto& declared : declared_hosts( self ) ) {
				if ( declared == host ) {
					return true;
				}
			}

			reason = "'" + host + "' is not in this extension's declared net hosts";

			return false;
		}

	}

	auto handle_net_get( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.net.get expects a URL string" );
			lua_error( state );
		}

		auto length = std::size_t{ 0 };
		const auto* url_text = lua_tolstring( state, 1, &length );
		const auto url = std::string{ url_text != nullptr ? url_text : "", length };

		auto denial = std::string{ };

		if ( !host_allowed( *self, url, denial ) ) {
			// A policy refusal is environmental failure, not a contract
			// violation: the extension can handle `nil, err`.
			lua_pushnil( state );
			lua_pushlstring( state, denial.data( ), denial.size( ) );

			return 2;
		}

		auto request = mcode::net::http_request{ };
		request.url = url;
		request.method = "GET";
		request.timeout_seconds = NET_TIMEOUT_SECONDS;

		auto* client = self->http_client( );

		if ( client == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no HTTP client was installed with this surface" );

			return 2;
		}

		client->set_max_response_bytes( NET_MAX_RESPONSE_BYTES );

		const auto response = client->send( request );

		if ( !response ) {
			lua_pushnil( state );
			lua_pushlstring( state, response.error( ).msg.data( ),
				response.error( ).msg.size( ) );

			return 2;
		}

		// The result is plain data: status, headers, body.
		lua_createtable( state, 0, 3 );

		lua_pushnumber( state, static_cast< double >( response->status ) );
		lua_setfield( state, -2, "status" );

		lua_createtable( state, 0, static_cast< int >( response->headers.size( ) ) );

		for ( const auto& [ name, value ] : response->headers ) {
			lua_pushlstring( state, name.data( ), name.size( ) );
			lua_pushlstring( state, value.data( ), value.size( ) );
			lua_rawset( state, -3 );
		}

		lua_setfield( state, -2, "headers" );

		lua_pushlstring( state, response->body.data( ), response->body.size( ) );
		lua_setfield( state, -2, "body" );

		return 1;
	}

	auto handle_net_search( lua_State* state ) -> int {
		auto* self = surface_from( state );

		if ( lua_type( state, 1 ) != LUA_TSTRING ) {
			lua_pushliteral( state, "mcode.net.search expects a query string" );
			lua_error( state );
		}

		auto length = std::size_t{ 0 };
		const auto* query_text = lua_tolstring( state, 1, &length );
		const auto query = std::string{ query_text != nullptr ? query_text : "", length };

		if ( query.empty( ) ) {
			lua_pushliteral( state, "mcode.net.search: the query must not be empty" );
			lua_error( state );
		}

		auto denial = std::string{ };

		if ( !manifest_allows( *self, "net" ) ) {
			denial = "permission denied: 'net' is required for mcode.net.search";

			lua_pushnil( state );
			lua_pushlstring( state, denial.data( ), denial.size( ) );

			return 2;
		}

		// Search is routed through the configured provider, which is a host
		// concern: the extension names WHAT to search, never WHERE. No host
		// allowlist applies because the extension does not choose the endpoint.
		auto* searcher = self->web_searcher( );

		if ( searcher == nullptr ) {
			lua_pushnil( state );
			lua_pushliteral( state, "no search provider was installed with this "
				"surface" );

			return 2;
		}

		const auto results = ( *searcher )( query );

		if ( !results ) {
			lua_pushnil( state );
			lua_pushlstring( state, results.error( ).msg.data( ),
				results.error( ).msg.size( ) );

			return 2;
		}

		lua_createtable( state, 0, static_cast< int >( results->size( ) ) );

		for ( auto index = std::size_t{ 0 }; index < results->size( ); ++index ) {
			const auto& entry = ( *results )[ index ];

			lua_createtable( state, 0, 3 );

			lua_pushlstring( state, entry.title.data( ), entry.title.size( ) );
			lua_setfield( state, -2, "title" );

			lua_pushlstring( state, entry.url.data( ), entry.url.size( ) );
			lua_setfield( state, -2, "url" );

			lua_pushlstring( state, entry.snippet.data( ), entry.snippet.size( ) );
			lua_setfield( state, -2, "snippet" );

			lua_rawseti( state, -2, static_cast< int >( index + 1 ) );
		}

		return 1;
	}

}
