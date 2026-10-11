#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mcode/tools/truncate.hxx"

#include "test_scratch.hxx"

#include "tools_test_helpers.hxx"

using namespace mcode;
using namespace mcode::tools;
using namespace tools_test;

TEST_CASE( "an over-cap result spills to an artifact with an actionable notice",
	"[tools][truncate]" ) {
	auto setup = fixture{ };

	auto big = std::string{ };

	for ( auto index = 0; index < 1000; ++index ) {
		big += "line of output that keeps going\n";
	}

	const auto out = truncate_result( big, setup.context.run_id, setup.space, "test" );
	CHECK( out.size( ) <= INLINE_RESULT_CHARS + 512 );
	CHECK( out.find( ".mcode/artifacts/test-run/test-" ) != std::string::npos );
	CHECK( out.find( "Full output written to" ) != std::string::npos );

	const auto path_start = out.find( ".mcode/artifacts/" );
	const auto path_end = out.find( ';', path_start );
	REQUIRE( path_end != std::string::npos );
	const auto spill_path = out.substr( path_start, path_end - path_start );

	const auto spilled = setup.space.read_file( spill_path );
	REQUIRE( spilled );
	CHECK( *spilled == big );
}

TEST_CASE( "two truncated results in one run spill to distinct artifacts",
	"[tools][truncate]" ) {
	auto setup = fixture{ };

	auto big = std::string{ };

	for ( auto index = 0; index < 1000; ++index ) {
		big += "line of output that keeps going\n";
	}

	const auto first = truncate_result( big, setup.context.run_id, setup.space, "bash" );
	const auto second = truncate_result( big, setup.context.run_id, setup.space, "bash" );

	CHECK( first.find( "truncation failed" ) == std::string::npos );
	CHECK( second.find( "truncation failed" ) == std::string::npos );

	const auto first_path = first.substr( first.find( ".mcode/artifacts/" ) );
	const auto second_path = second.substr( second.find( ".mcode/artifacts/" ) );
	CHECK( first_path != second_path );
}
