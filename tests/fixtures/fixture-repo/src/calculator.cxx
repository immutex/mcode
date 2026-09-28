#include "calculator.hxx"

namespace fixture {

	auto add( const int left, const int right ) -> int {
		return left + right;
	}

	auto divide( const int numerator, const int denominator ) -> int {
		return numerator / denominator;
	}

}
