#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <string_view>

#include "mcode/core/version.hxx"

TEST_CASE( "C++23 is the language standard", "[build]" ) {
	STATIC_REQUIRE( __cplusplus >= 202100L );
}

TEST_CASE( "the error model uses std::expected", "[build]" ) {
	// <expected> must be included before the macro is checked; `defined` needs a guard.
#if defined( __cpp_lib_expected )
	SUCCEED( "std::expected is available" );
#else
	FAIL( "std::expected is required" );
#endif
}

TEST_CASE( "version metadata is populated", "[build]" ) {
	REQUIRE_FALSE( mcode::VERSION.empty( ) );
	REQUIRE_FALSE( mcode::PLATFORM.empty( ) );
	REQUIRE_FALSE( mcode::COMPILER.empty( ) );

	REQUIRE( mcode::VERSION.find( '@' ) == std::string_view::npos );
	REQUIRE( mcode::PLATFORM.find( '@' ) == std::string_view::npos );
}

TEST_CASE( "exactly one platform macro is defined", "[build]" ) {
	constexpr int defined =
#if defined( MCODE_OS_WINDOWS )
		1;
#elif defined( MCODE_OS_LINUX )
		1;
#elif defined( MCODE_OS_MACOS )
		1;
#else
		0;
#endif

	STATIC_REQUIRE( defined == 1 );
}
