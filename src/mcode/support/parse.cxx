#include "mcode/support/parse.hxx"

#include <cmath>
#include <cstdint>

namespace mcode::support {

	namespace {

		// A significand with more digits than this cannot be represented exactly, so
		// the value would be a rounded guess. Refusing is the honest answer, and the
		// bound also keeps the accumulator inside a 64-bit integer.
		constexpr auto MAX_SIGNIFICAND_DIGITS = 18;

		// The exponent loop below runs this many times at most. Anything beyond is
		// an overflow or an underflow, and both are refusals.
		constexpr auto MAX_DECIMAL_EXPONENT = 400;

		auto is_digit( const char character ) -> bool {
			return character >= '0' && character <= '9';
		}

	}

	auto parse_double( const std::string_view text, double& out ) -> bool {
		auto index = std::size_t{ 0 };

		if ( index < text.size( ) && ( text[ index ] == '+' || text[ index ] == '-' ) ) {
			++index;
		}

		// The significand accumulates as an integer, with the decimal point's
		// position recorded as an exponent. That is what makes the conversion
		// locale-free and exact for any value a double can hold distinctly.
		auto significand = std::uint64_t{ 0 };
		auto digits = std::size_t{ 0 };
		auto exponent = 0;
		auto negative = text.starts_with( '-' );

		while ( index < text.size( ) && is_digit( text[ index ] ) ) {
			significand = significand * 10 + static_cast< std::uint64_t >( text[ index ] - '0' );
			++digits;
			++index;
		}

		// TOML requires at least one digit before the point.
		if ( digits == 0 ) {
			return false;
		}

		if ( index < text.size( ) && text[ index ] == '.' ) {
			++index;

			auto fractional = std::size_t{ 0 };

			while ( index < text.size( ) && is_digit( text[ index ] ) ) {
				significand = significand * 10 + static_cast< std::uint64_t >( text[ index ] - '0' );
				++digits;
				++fractional;
				++index;
			}

			// A point with no digits after it is malformed, not a whole number.
			if ( fractional == 0 ) {
				return false;
			}

			exponent -= static_cast< int >( fractional );
		}

		if ( index < text.size( ) && ( text[ index ] == 'e' || text[ index ] == 'E' ) ) {
			++index;

			auto exponent_negative = false;

			if ( index < text.size( ) && ( text[ index ] == '+' || text[ index ] == '-' ) ) {
				exponent_negative = text[ index ] == '-';
				++index;
			}

			auto exponent_digits = 0;
			auto explicit_exponent = 0;

			while ( index < text.size( ) && is_digit( text[ index ] ) ) {
				// Bounded before the multiply: an exponent long enough to overflow
				// would be undefined behaviour, and the value is untrusted input.
				if ( explicit_exponent > MAX_DECIMAL_EXPONENT ) {
					return false;
				}

				explicit_exponent = explicit_exponent * 10 + ( text[ index ] - '0' );
				++exponent_digits;
				++index;
			}

			// An exponent marker with no digits is malformed.
			if ( exponent_digits == 0 ) {
				return false;
			}

			exponent += exponent_negative ? -explicit_exponent : explicit_exponent;
		}

		// Trailing characters mean the literal is not entirely numeric.
		if ( index != text.size( ) ) {
			return false;
		}

		if ( digits > MAX_SIGNIFICAND_DIGITS || exponent > MAX_DECIMAL_EXPONENT ||
			exponent < -MAX_DECIMAL_EXPONENT ) {
			return false;
		}

		auto value = static_cast< double >( significand );

		// One multiplication or division per decade. Each is a single rounding, and
		// the loop is bounded by the exponent check above.
		for ( auto step = 0; step < exponent; ++step ) {
			value *= 10.0;
		}

		for ( auto step = 0; step > exponent; --step ) {
			value /= 10.0;
		}

		// Overflow became an infinity and underflow became zero. Neither is the
		// value the author wrote, so both are refusals.
		if ( !std::isfinite( value ) ) {
			return false;
		}

		if ( value == 0.0 && significand != 0 ) {
			return false;
		}

		out = negative ? -value : value;

		return true;
	}

}
