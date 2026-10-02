#pragma once

#include <string_view>
#include <vector>

namespace mcode::support {

	[[nodiscard]] auto glob_segments( std::string_view text )
		-> std::vector< std::string_view >;

	[[nodiscard]] auto wildcard_match( std::string_view pattern,
		std::string_view text ) -> bool;

	// `**` consumes zero or more segments; every other segment matches exactly one.
	[[nodiscard]] auto glob_match( std::string_view pattern,
		std::string_view path ) -> bool;

}
