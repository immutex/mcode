#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "mcode/core/error.hxx"
#include "mcode/model/provider.hxx"

namespace mcode::model {

	[[nodiscard]] auto auth_header_value( const auth_spec& auth, std::string_view api_key )
		-> result< std::string >;

	[[nodiscard]] auto resolve_api_key( const auth_spec& auth,
		const std::function< std::optional< std::string >( std::string_view ) >& from_config )
		-> result< std::string >;

}
