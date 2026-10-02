#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "mcode/eval/suite.hxx"
#include "mcode/support/json.hxx"

using namespace mcode;

namespace {

	auto fixture_root( ) -> std::filesystem::path {
		return std::filesystem::path{ MCODE_FIXTURE_REPO };
	}

}

TEST_CASE( "pass@k and pass^k are different questions", "[eval]" ) {
	// pass@k alone flatters a harness that works half the time, so reliability is required too
	const auto all_passed = std::vector< bool >{ true, true, true };
	const auto some_passed = std::vector< bool >{ true, false, true };
	const auto none_passed = std::vector< bool >{ false, false, false };

	REQUIRE( eval::pass_at_k( all_passed ) == 1.0 );
	REQUIRE( eval::pass_at_k( some_passed ) == 1.0 );
	REQUIRE( eval::pass_at_k( none_passed ) == 0.0 );

	REQUIRE( eval::pass_power_k( all_passed ) == 1.0 );
	REQUIRE( eval::pass_power_k( some_passed ) == 0.0 );
	REQUIRE( eval::pass_power_k( none_passed ) == 0.0 );

	REQUIRE( eval::pass_at_k( some_passed ) != eval::pass_power_k( some_passed ) );

	REQUIRE( eval::pass_at_k( { } ) == 0.0 );
	REQUIRE( eval::pass_power_k( { } ) == 0.0 );
}

TEST_CASE( "the fixture repo exists and has the expected shape", "[eval]" ) {
	const auto root = fixture_root( );

	REQUIRE( std::filesystem::exists( root ) );
	REQUIRE( std::filesystem::exists( root / "CMakeLists.txt" ) );
	REQUIRE( std::filesystem::exists( root / "src" / "calculator.cxx" ) );
	REQUIRE( std::filesystem::exists( root / "include" / "calculator.hxx" ) );
	REQUIRE( std::filesystem::exists( root / "tests" / "test_calculator.cxx" ) );

	REQUIRE( std::filesystem::exists( root / ".mcode" / "config.toml" ) );
}

TEST_CASE( "the builtin suite has ten deterministic tasks with unique ids", "[eval]" ) {
	const auto tasks = eval::builtin_tasks( );

	REQUIRE( tasks.size( ) == 10 );

	auto ids = std::vector< std::string >{ };

	for ( const auto& entry : tasks ) {
		REQUIRE_FALSE( entry.id.empty( ) );
		REQUIRE_FALSE( entry.description.empty( ) );

		// a task body that cannot execute is a broken suite
		auto outcome = entry.run( fixture_root( ) );

		REQUIRE( static_cast< bool >( outcome ) );

		if ( outcome ) {
			REQUIRE( *outcome );
		}

		ids.push_back( entry.id );
	}

	std::sort( ids.begin( ), ids.end( ) );
	REQUIRE( std::adjacent_find( ids.begin( ), ids.end( ) ) == ids.end( ) );
}

TEST_CASE( "every task passes against the fixture repo", "[eval]" ) {
	const auto result = eval::run_suite( fixture_root( ) );

	REQUIRE( result.total( ) == 10 );
	REQUIRE( result.all_passed( ) );

	if ( !result.all_passed( ) ) {
		for ( const auto& record : result.records ) {
			if ( record.verdict != "pass" ) {
				FAIL( record.task_id << ": " << record.verdict << " -- " << record.detail );
			}
		}
	}
}

TEST_CASE( "the suite is deterministic across runs", "[eval]" ) {
	const auto first = eval::run_suite( fixture_root( ) );
	const auto second = eval::run_suite( fixture_root( ) );

	REQUIRE( first.records.size( ) == second.records.size( ) );

	for ( auto index = std::size_t{ 0 }; index < first.records.size( ); ++index ) {
		REQUIRE( first.records[ index ].task_id == second.records[ index ].task_id );
		REQUIRE( first.records[ index ].verdict == second.records[ index ].verdict );
	}
}

TEST_CASE( "run records serialize to parseable JSONL", "[eval]" ) {
	const auto result = eval::run_suite( fixture_root( ) );
	const auto jsonl = eval::to_jsonl( result );

	auto lines = std::vector< std::string >{ };
	auto start = std::size_t{ 0 };

	while ( start < jsonl.size( ) ) {
		const auto end = jsonl.find( '\n', start );

		if ( end == std::string::npos ) {
			break;
		}

		lines.push_back( jsonl.substr( start, end - start ) );
		start = end + 1;
	}

	REQUIRE( lines.size( ) == result.total( ) );

	for ( const auto& line : lines ) {
		// records are consumed by tooling so each must be valid JSON
		auto parsed = json::document::parse( line );
		REQUIRE( static_cast< bool >( parsed ) );

		if ( !parsed ) {
			FAIL( "unparseable record: " << line );
		}

		REQUIRE( parsed->get_string( "run_id" ).has_value( ) );
		REQUIRE( parsed->get_string( "task_id" ).has_value( ) );
		REQUIRE( parsed->get_string( "suite" ).has_value( ) );
		REQUIRE( parsed->pointer_string( "/outcome/verdict" ).has_value( ) );
		REQUIRE( parsed->pointer_int( "/outcome/exit_code" ).has_value( ) );
	}
}

TEST_CASE( "a detail message with quotes still produces valid JSON", "[eval]" ) {
	auto record = eval::run_record{ };
	record.run_id = "r";
	record.timestamp = "t";
	record.suite = "s";
	record.task_id = "x";
	record.scaffold_revision = "rev";
	record.verdict = "error";
	record.detail = R"(rejected "C:\path\file.toml": bad "key")";

	auto parsed = json::document::parse( record.to_json( ) );
	REQUIRE( static_cast< bool >( parsed ) );

	auto detail = parsed->get_string( "detail" );
	REQUIRE( detail.has_value( ) );
	REQUIRE( *detail == record.detail );
}
