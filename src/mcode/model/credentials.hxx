#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"
#include "mcode/model/provider.hxx"

namespace mcode::model {

	// The auth header value for a descriptor, or empty when the source is
	// `none`. `api_key` is passed in rather than looked up, so this stays a
	// pure function and the resolution policy lives in one place.
	//
	// The value is `scheme + " " + key` when the scheme is non-empty, else the
	// bare key: Anthropic's descriptor uses `x-api-key` with no scheme.
	[[nodiscard]] auto auth_header_value( const auth_spec& auth, std::string_view api_key )
		-> result< std::string >;

	// Resolves a descriptor's credential from the environment or the config.
	// Fails when the source is set but the value is missing -- a request sent
	// without its credential is a 401 the user has to decode.
	[[nodiscard]] auto resolve_api_key( const auth_spec& auth,
		const std::function< std::optional< std::string >( std::string_view ) >& from_config )
		-> result< std::string >;

}
