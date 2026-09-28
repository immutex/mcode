#include "calculator.hxx"

#include <cstdio>

auto main( ) -> int {
	auto failures = 0;

	if ( fixture::add( 2, 3 ) != 5 ) {
		std::printf( "add failed\n" );
		++failures;
	}

	if ( fixture::divide( 10, 2 ) != 5 ) {
		std::printf( "divide failed\n" );
		++failures;
	}

	return failures;
}
