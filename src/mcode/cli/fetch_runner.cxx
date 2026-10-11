#include "mcode/cli/fetch_runner.hxx"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "mcode/net/egress_policy.hxx"

namespace mcode::cli {

	namespace {

		constexpr std::int64_t MAX_TIMEOUT_SECONDS = 120;
		constexpr std::uint64_t FETCH_MAX_RESPONSE_BYTES = 4ull * 1024ull * 1024ull;
		constexpr std::uint64_t FETCH_MAX_BODY_BYTES = 256ull * 1024ull;

		// A body the model cannot read as text is not a body: returning binary bytes
		// would spend context on mojibake, so the type is checked before the read.
		[[nodiscard]] auto is_textual( const std::string_view content_type ) -> bool {
			if ( content_type.empty( ) ) {
				// Absent a type, the caller asked for something and most servers that
				// omit it are serving text.
				return true;
			}

			const auto lowered = [ &content_type ]( ) {
				auto out = std::string{ content_type };

				for ( auto& character : out ) {
					if ( character >= 'A' && character <= 'Z' ) {
						character = static_cast< char >( character - 'A' + 'a' );
					}
				}

				return out;
			}( );

			constexpr auto TEXTUAL = std::array{ "text/", "application/json",
				"application/xml", "application/xhtml", "application/javascript",
				"application/x-yaml", "application/yaml" };

			for ( const auto prefix : TEXTUAL ) {
				if ( lowered.find( prefix ) != std::string::npos ) {
					return true;
				}
			}

			return false;
		}

		[[nodiscard]] auto header_of(
			const std::map< std::string, std::string, std::less<> >& headers,
			const std::string_view wanted ) -> std::string {
			for ( const auto& [ name, value ] : headers ) {
				if ( name.size( ) != wanted.size( ) ) {
					continue;
				}

				auto equal = true;

				for ( auto index = std::size_t{ 0 }; index < name.size( ); ++index ) {
					const auto byte = static_cast< unsigned char >( name[ index ] );
					const auto lowered = static_cast< char >(
						( byte >= 'A' && byte <= 'Z' ) ? byte + ( 'a' - 'A' ) : byte );

					if ( lowered != wanted[ index ] ) {
						equal = false;

						break;
					}
				}

				if ( equal ) {
					return value;
				}
			}

			return { };
		}

	}

	auto make_fetch_runner( const mcode::config::merged_config& config,
		mcode::net::http_client& client, mcode::perm::permission_engine* permissions )
		-> std::function< mcode::result< mcode::ext::fetch_result >( const std::string&,
			std::int64_t ) > {
		// `[web] fetch = false` removes the capability entirely, which is how a user
		// who does not want model-chosen egress turns it off without disabling `web`.
		const auto enabled = config.get_bool( "web.fetch" ).value_or( true );

		if ( !enabled ) {
			return { };
		}

		return [ &client, permissions ]( const std::string& url, const std::int64_t timeout_ms )
			-> mcode::result< mcode::ext::fetch_result > {
			const auto parsed = mcode::net::parse_url( url );

			if ( !parsed ) {
				return std::unexpected( mcode::fail( mcode::errc::config,
					"the URL could not be parsed" ) );
			}

			if ( parsed->scheme != "http" && parsed->scheme != "https" ) {
				return std::unexpected( mcode::fail( mcode::errc::config,
					"only http and https are fetchable" ) );
			}

			// Fail closed: a fetch with no engine behind it is an ungated egress path.
			if ( permissions == nullptr ) {
				return std::unexpected( mcode::fail( mcode::errc::tool_failed,
					"no permission engine is available to gate this fetch" ) );
			}

			// The literal-address case, refused ahead of the decision so a URL naming
			// a private address never reaches the prompt with an allow.
			if ( mcode::net::egress_policy::address_is_denied( parsed->host ) ) {
				return std::unexpected( mcode::fail( mcode::errc::tool_failed,
					"'" + parsed->host + "' is a private or reserved address" ) );
			}

			// Per domain, through the same engine every other tool is gated by: a
			// rule the user wrote for `fetch` applies here, and `permissions.deny`
			// is a hard stop.
			auto request = mcode::perm::permission_request{ };
			request.tool_name = "fetch";
			request.klass = mcode::tool_class::net;
			request.owner = "web";
			request.resource = parsed->host;

			if ( permissions->decide( request ) == mcode::perm::permission_decision::deny ) {
				const auto& verdict = permissions->last_verdict( );

				return std::unexpected( mcode::fail( mcode::errc::tool_failed,
					verdict.reason.empty( ) ? "denied by policy" : verdict.reason ) );
			}

			auto http_request = mcode::net::http_request{ };
			http_request.url = url;
			http_request.method = "GET";
			http_request.timeout_seconds =
				timeout_ms > 0 ? timeout_ms / 1000 : MAX_TIMEOUT_SECONDS;

			if ( http_request.timeout_seconds > MAX_TIMEOUT_SECONDS ) {
				http_request.timeout_seconds = MAX_TIMEOUT_SECONDS;
			}

			http_request.headers[ "user-agent" ] = "mcode";
			http_request.headers[ "accept" ] = "text/html,application/json,text/plain,*/*";

			client.set_max_response_bytes( FETCH_MAX_RESPONSE_BYTES );

			const auto response = client.send( http_request );

			if ( !response ) {
				return std::unexpected( response.error( ) );
			}

			auto result = mcode::ext::fetch_result{ };
			result.status = response->status;
			result.content_type = header_of( response->headers, "content-type" );

			if ( !is_textual( result.content_type ) ) {
				return std::unexpected( mcode::fail( mcode::errc::tool_failed,
					"'" + result.content_type + "' is not a text content type, so the "
					"body was not read" ) );
			}

			// The cap is on what enters the context, not on what was downloaded: the
			// client's own limit is the memory guard, this one is the budget guard.
			if ( response->body.size( ) > FETCH_MAX_BODY_BYTES ) {
				result.body = response->body.substr( 0, FETCH_MAX_BODY_BYTES );
				result.truncated = true;
			} else {
				result.body = response->body;
			}

			return result;
		};
	}

}
