#include "mcode/support/parse.hxx"

#include <cerrno>
#include <cstdlib>
#include <string>

namespace mcode::support {

	auto parse_double( const std::string_view text, double& out ) -> bool {
		if ( text.empty( ) ) {
			return false;
		}

		// strtod wants a NUL-terminated buffer and reports where it stopped, which
		// is the whole check: anything left unconsumed is not a number.
		const auto buffer = std::string{ text };

		errno = 0;

		auto* end = static_cast< char* >( nullptr );
		const auto value = std::strtod( buffer.c_str( ), &end );

		if ( end != buffer.c_str( ) + buffer.size( ) ) {
			return false;
		}

		// ERANGE means the value did not fit, which is a refusal rather than a
		// saturated number.
		if ( errno == ERANGE ) {
			return false;
		}

		out = value;

		return true;
	}

}
