#pragma once

#include <string_view>

namespace mcode::support {

	// not std::from_chars (Xcode 15's libc++ lacks the float overload) nor std::stod.
	[[nodiscard]] auto parse_double( std::string_view text, double& out ) -> bool;

}
