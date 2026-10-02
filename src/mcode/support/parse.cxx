#include "mcode/support/parse.hxx"

#include <cmath>
#include <cstdint>

namespace mcode::support {

	namespace {

		constexpr auto MAX_SIGNIFICAND_DIGITS = 18;

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

		auto significand = std::uint64_t{ 0 };
		auto digits = std::size_t{ 0 };
		auto exponent = 0;
		auto negative = text.starts_with( '-' );

		while ( index < text.size( ) && is_digit( text[ index ] ) ) {
			significand = significand * 10 + static_cast< std::uint64_t >( text[ index ] - '0' );
			++digits;
			++index;
		}

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
				// bounded before the multiply: an overflowing exponent is UB.
				if ( explicit_exponent > MAX_DECIMAL_EXPONENT ) {
					return false;
				}

				explicit_exponent = explicit_exponent * 10 + ( text[ index ] - '0' );
				++exponent_digits;
				++index;
			}

			if ( exponent_digits == 0 ) {
				return false;
			}

			exponent += exponent_negative ? -explicit_exponent : explicit_exponent;
		}

		if ( index != text.size( ) ) {
			return false;
		}

		if ( digits > MAX_SIGNIFICAND_DIGITS || exponent > MAX_DECIMAL_EXPONENT ||
			exponent < -MAX_DECIMAL_EXPONENT ) {
			return false;
		}

		auto value = static_cast< double >( significand );

		// one multiplication per decade, each a single rounding.
		for ( auto step = 0; step < exponent; ++step ) {
			value *= 10.0;
		}

		for ( auto step = 0; step > exponent; --step ) {
			value /= 10.0;
		}

		// overflow became infinity and underflow became zero.
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
