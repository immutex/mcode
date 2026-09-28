#pragma once

#include <string_view>

namespace mcode::support {

	// Parses a floating-point number, refusing anything that is not entirely
	// numeric. Returns false on trailing characters, on an empty input, and on
	// overflow/underflow.
	//
	// Not std::from_chars: the floating-point overload is C++17 on paper, but
	// Xcode 15's libc++ does not implement it, so a build that compiles here fails
	// on macOS. Not std::stod either: it stops at the first character it cannot use
	// and returns the prefix WITHOUT raising, so "0.25.9" silently becomes 0.25 and
	// "1e" becomes 1.0. A malformed number must be refused, not quietly replaced by
	// a different one.
	//
	// Locale: strtod reads the decimal point from the current locale. mcode never
	// calls setlocale, so the C locale applies. Under a comma-decimal locale the
	// result is a REFUSAL (the fraction is left unconsumed), not a wrong value,
	// which is the correct direction for a config parser.
	[[nodiscard]] auto parse_double( std::string_view text, double& out ) -> bool;

}
