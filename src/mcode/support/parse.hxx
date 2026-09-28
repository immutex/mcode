#pragma once

#include <string_view>

namespace mcode::support {

	// Parses a decimal floating-point literal, refusing anything malformed.
	//
	// Not `std::from_chars`: the floating-point overload is C++17 on paper, but
	// Xcode 15's libc++ does not implement it, so a build that passes here fails on
	// macOS. Not `std::stod` either: it stops at the first character it cannot use
	// and returns the prefix WITHOUT raising, so "0.25.9" silently becomes 0.25 and
	// "1e" becomes 1.0 -- a malformed number quietly replaced by a different one.
	//
	// The grammar is the TOML subset the parser already accepts:
	//
	//     [+-]? digits [ '.' digits ] [ ('e'|'E') [+-]? digits ]
	//
	// Digits are required on both sides of a decimal point, and an exponent must
	// carry at least one digit. Conversion is explicit rather than delegated to
	// `strtod`, so the result does not depend on `LC_NUMERIC`: a locale whose
	// decimal separator is a comma would make `strtod` read "0.25" as 0.
	//
	// Returns false on a malformed literal, on more digits than a double can hold
	// distinctly, and on a value outside the finite range. A refusal is always
	// preferable to a saturated or silently-truncated number.
	[[nodiscard]] auto parse_double( std::string_view text, double& out ) -> bool;

}
