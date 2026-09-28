#include "mcode/model/credentials.hxx"

#include <cstdlib>
#include <string>

namespace mcode::model {

	auto auth_header_value( const auth_spec& auth, const std::string_view api_key )
		-> result< std::string > {
		if ( auth.from == auth_spec::source::none ) {
			return std::string{ };
		}

		if ( api_key.empty( ) ) {
			return std::unexpected( fail( errc::config,
				"provider auth source is set but the key is empty; a request without its "
				"credential would fail with a 401 the user has to decode" ) );
		}

		if ( auth.scheme.empty( ) ) {
			return std::string{ api_key };
		}

		return std::string{ auth.scheme } + " " + std::string{ api_key };
	}

	auto resolve_api_key( const auth_spec& auth,
		const std::function< std::optional< std::string >( std::string_view ) >& from_config )
		-> result< std::string > {
		switch ( auth.from ) {
			case auth_spec::source::none:
				return std::string{ };

			case auth_spec::source::environment: {
				const auto* value = std::getenv( auth.name.c_str( ) );

				if ( value == nullptr || *value == '\0' ) {
					return std::unexpected( fail( errc::config,
						"environment variable '" + auth.name + "' is not set, and the provider's "
						"credential comes from it" ) );
				}

				return std::string{ value };
			}

			case auth_spec::source::config: {
				if ( !from_config ) {
					return std::unexpected( fail( errc::config,
						"provider auth reads config but no config source was supplied" ) );
				}

				auto value = from_config( auth.name );

				if ( !value || value->empty( ) ) {
					return std::unexpected( fail( errc::config,
						"config key '" + auth.name + "' is not set, and the provider's "
						"credential comes from it" ) );
				}

				return *value;
			}
		}

		return std::unexpected( fail( errc::config, "unknown auth source" ) );
	}

}
