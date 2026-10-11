#include "mcode/cli/web_search.hxx"

#include <cstdint>
#include <string>
#include <vector>

#include "mcode/support/json.hxx"

namespace mcode::cli {

	namespace {

		constexpr std::int64_t SEARCH_TIMEOUT_SECONDS = 20;
		constexpr std::uint64_t SEARCH_MAX_RESPONSE_BYTES = 4ull * 1024ull * 1024ull;
		constexpr std::size_t MAX_RESULTS = 20;
		constexpr std::size_t MAX_SNIPPET_CHARS = 512;

		// A query is the model's text, so it is encoded rather than interpolated: an
		// unencoded `&` in a query silently becomes a second parameter.
		[[nodiscard]] auto url_encode( const std::string_view text ) -> std::string {
			auto out = std::string{ };
			constexpr auto HEX = "0123456789ABCDEF";

			for ( const auto character : text ) {
				const auto byte = static_cast< unsigned char >( character );

				if ( ( byte >= 'A' && byte <= 'Z' ) || ( byte >= 'a' && byte <= 'z' )
					|| ( byte >= '0' && byte <= '9' ) || byte == '-' || byte == '_'
					|| byte == '.' || byte == '~' ) {
					out += static_cast< char >( byte );

					continue;
				}

				out += '%';
				out += HEX[ byte >> 4 ];
				out += HEX[ byte & 0x0f ];
			}

			return out;
		}

		// JSON string escaping, not URL escaping: the same query goes into two contexts.
		[[nodiscard]] auto json_escape( const std::string_view text ) -> std::string {
			auto out = std::string{ };
			json::append_escaped( out, text );

			return out;
		}

		[[nodiscard]] auto substitute( const std::string_view template_text,
			const std::string_view query, const bool url_context ) -> std::string {
			constexpr auto TOKEN = std::string_view{ "{query}" };

			auto out = std::string{ };
			auto rest = template_text;

			while ( true ) {
				const auto at = rest.find( TOKEN );

				if ( at == std::string_view::npos ) {
					out += rest;

					break;
				}

				out += rest.substr( 0, at );
				out += url_context ? url_encode( query ) : json_escape( query );
				rest = rest.substr( at + TOKEN.size( ) );
			}

			return out;
		}

		// The provider's shape lives in config, not in this file: the harness supplies
		// the request and the caps, and nothing here knows what Tavily or Brave is.
		struct provider_config {
			std::string url;
			std::string method;
			std::string body;
			std::string headers;
			std::string api_key_env;

			std::string results_path;
			std::string title_path;
			std::string url_path;
			std::string snippet_path;
		};

		[[nodiscard]] auto read_provider( const mcode::config::merged_config& config )
			-> provider_config {
			auto out = provider_config{ };

			const auto string_or = [ &config ]( const std::string_view key ) -> std::string {
				return config.get_string( key ).value_or( std::string{ } );
			};

			out.url = string_or( "web.search_url" );
			out.method = string_or( "web.search_method" );
			out.body = string_or( "web.search_body" );
			out.headers = string_or( "web.search_headers" );
			out.api_key_env = string_or( "web.search_api_key_env" );

			out.results_path = string_or( "web.results_path" );
			out.title_path = string_or( "web.title_path" );
			out.url_path = string_or( "web.url_path" );
			out.snippet_path = string_or( "web.snippet_path" );

			return out;
		}

		[[nodiscard]] auto read_string_at( const json::document& document,
			const std::string_view path ) -> std::string {
			if ( path.empty( ) ) {
				return { };
			}

			return document.pointer_string( path ).value_or( std::string{ } );
		}

	}

	auto make_web_searcher( const mcode::config::merged_config& config,
		mcode::net::http_client& client )
		-> std::function< mcode::ext::search_result( const std::string& ) > {
		const auto provider = read_provider( config );

		// Absent configuration means no searcher at all, so `web_search` is absent from
		// the tool list rather than present and failing. Shipping a default endpoint
		// would be an opinion about a vendor and a key we cannot supply.
		if ( provider.url.empty( ) ) {
			return { };
		}

		const auto has_body = !provider.body.empty( );

		return [ &client, provider, has_body ]( const std::string& query )
			-> mcode::ext::search_result {
			auto request = mcode::net::http_request{ };
			request.url = substitute( provider.url, query, true );
			request.method = has_body ? "POST" : "GET";
			request.timeout_seconds = SEARCH_TIMEOUT_SECONDS;

			if ( has_body ) {
				request.body = substitute( provider.body, query, false );
				request.headers[ "content-type" ] = "application/json";
			}

			if ( !provider.headers.empty( ) ) {
				const auto extra = json::document::parse( provider.headers );

				if ( !extra ) {
					return std::unexpected( mcode::fail( mcode::errc::config,
						"web.search_headers is not valid JSON" ) );
				}

				for ( const auto& name : extra->keys_at( "/" ) ) {
					request.headers[ name ] = extra->pointer_string( "/" + name ).value_or(
						std::string{ } );
				}
			}

			// The key is read here and never handed to an extension: a credential an
			// extension can name is a credential it can exfiltrate.
			if ( !provider.api_key_env.empty( ) ) {
				if ( const auto* key = std::getenv( provider.api_key_env.c_str( ) );
					key != nullptr ) {
					request.headers[ "authorization" ] = std::string{ "Bearer " } + key;
				}
			}

			client.set_max_response_bytes( SEARCH_MAX_RESPONSE_BYTES );

			const auto response = client.send( request );

			if ( !response ) {
				return std::unexpected( response.error( ) );
			}

			if ( response->status < 200 || response->status >= 300 ) {
				return std::unexpected( mcode::fail( mcode::errc::protocol,
					"the search provider answered HTTP "
						+ std::to_string( response->status ) ) );
			}

			const auto parsed = json::document::parse( response->body );

			if ( !parsed ) {
				return std::unexpected( mcode::fail( mcode::errc::protocol,
					"the search provider answered with invalid JSON" ) );
			}

			if ( !parsed->has_pointer( provider.results_path ) ) {
				return std::unexpected( mcode::fail( mcode::errc::protocol,
					"web.results_path '" + provider.results_path
						+ "' does not name an array in the response" ) );
			}

			// Each row is fetched by index and re-parsed, so a nested shape (Tavily's
			// `{title,url,content}`) and a flat one are the same code path. The index
			// walk stops at the first missing element, which is the array's end.
			auto out = std::vector< mcode::ext::search_hit >{ };

			for ( auto index = std::size_t{ 0 }; index < MAX_RESULTS; ++index ) {
				const auto row_path = provider.results_path + "/" + std::to_string( index );
				const auto row = parsed->pointer_raw( row_path );

				if ( !row ) {
					break;
				}

				const auto entry = json::document::parse( *row );

				if ( !entry ) {
					continue;
				}

				auto hit = mcode::ext::search_hit{ };
				hit.title = read_string_at( *entry, provider.title_path );
				hit.url = read_string_at( *entry, provider.url_path );
				hit.snippet = read_string_at( *entry, provider.snippet_path );

				if ( hit.snippet.size( ) > MAX_SNIPPET_CHARS ) {
					hit.snippet.resize( MAX_SNIPPET_CHARS );
				}

				if ( hit.url.empty( ) && hit.title.empty( ) ) {
					continue;
				}

				out.push_back( std::move( hit ) );
			}

			return out;
		};
	}

}
