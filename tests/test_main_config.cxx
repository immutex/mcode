#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <string_view>

#include "mcode/core/version.hxx"

TEST_CASE( "C++23 is the language standard", "[build]" ) {
	STATIC_REQUIRE( __cplusplus >= 202100L );
}

TEST_CASE( "the error model uses std::expected", "[build]" ) {
	// The macro is defined by <expected>, so that header must be included before
	// the macro is tested. Without the include this reported "std::expected is
	// required" on a toolchain that has it.
	// `defined` is only valid in a preprocessor conditional, so this stays an
	// #ifdef rather than a STATIC_REQUIRE.
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
