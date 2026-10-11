#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "mcode/agent/loop.hxx"
#include "mcode/eval/suite.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	auto scratch( const std::string_view name ) -> std::filesystem::path {
		const auto directory = test::scratch_directory( "mcode-verdict-test" ) / name;
		std::filesystem::remove_all( directory );
		std::filesystem::create_directories( directory );

		return directory;
	}

	auto write_log( const std::filesystem::path& directory, const std::string_view name,
		const std::string_view content ) -> void {
		std::filesystem::create_directories( directory );

		auto out = std::ofstream{ directory / name, std::ios::trunc };
		out << content;
	}

	auto run_end_line( const std::string_view verdict ) -> std::string {
		return std::string{ "{\"v\":1,\"seq\":9,\"kind\":\"run.end\",\"payload\":{\"goal\":\"x\","
			"\"verification\":\"" } + std::string{ verdict } + "\",\"state\":\"done\"}}\n";
	}

}

TEST_CASE( "the verdict vocabulary is distinct and stable", "[verdict]" ) {
	REQUIRE( to_string( verification_verdict::no_tests ) == "no-tests" );
	REQUIRE( to_string( verification_verdict::verified ) == "verified" );
	REQUIRE( to_string( verification_verdict::failed ) == "failed" );
}

TEST_CASE( "a run defaults to no-tests, never to verified", "[verdict]" ) {
	const auto outcome = turn_outcome{ };

	// A run whose completion was never checked must not read as verified by omission.
	REQUIRE( outcome.verdict == verification_verdict::no_tests );
}

TEST_CASE( "false_success_rate counts unverified successes", "[eval][audit]" ) {
	auto audit = eval::session_audit{ };
	audit.verified = 3;
	audit.no_tests = 1;
	audit.failed = 5;

	// Failures are not successes, so they are not in the denominator.
	REQUIRE( audit.false_success_rate( ) == 0.25 );
}

TEST_CASE( "false_success_rate is zero with no successes rather than undefined", "[eval][audit]" ) {
	const auto audit = eval::session_audit{ };

	REQUIRE( audit.false_success_rate( ) == 0.0 );
}

TEST_CASE( "audit_sessions reads the verdict out of real logs", "[eval][audit]" ) {
	const auto directory = scratch( "audit-mixed" );

	write_log( directory, "a.jsonl", run_end_line( "verified" ) );
	write_log( directory, "b.jsonl", run_end_line( "no-tests" ) );
	write_log( directory, "c.jsonl", run_end_line( "failed" ) );

	const auto audit = eval::audit_sessions( directory );

	REQUIRE( audit.has_value( ) );
	REQUIRE( audit->runs == 3 );
	REQUIRE( audit->verified == 1 );
	REQUIRE( audit->no_tests == 1 );
	REQUIRE( audit->failed == 1 );
	REQUIRE( audit->unreadable == 0 );
	REQUIRE( audit->false_success_rate( ) == 0.5 );
}

TEST_CASE( "a run.end without a verdict counts as unverified", "[eval][audit]" ) {
	const auto directory = scratch( "audit-legacy" );

	// A record with no verification field cannot support a verified claim.
	write_log( directory, "old.jsonl",
		"{\"v\":1,\"seq\":9,\"kind\":\"run.end\",\"payload\":{\"state\":\"done\"}}\n" );

	const auto audit = eval::audit_sessions( directory );

	REQUIRE( audit.has_value( ) );
	REQUIRE( audit->runs == 1 );
	REQUIRE( audit->verified == 0 );
	REQUIRE( audit->no_tests == 1 );
	REQUIRE( audit->false_success_rate( ) == 1.0 );
}

TEST_CASE( "an unreadable run.end is counted, not silently skipped", "[eval][audit]" ) {
	const auto directory = scratch( "audit-corrupt" );

	write_log( directory, "broken.jsonl",
		"{\"kind\":\"run.end\",\"payload\":{\"verification\":\"verified\"\n" );
	write_log( directory, "good.jsonl", run_end_line( "verified" ) );

	const auto audit = eval::audit_sessions( directory );

	REQUIRE( audit.has_value( ) );
	REQUIRE( audit->verified == 1 );
	REQUIRE( audit->unreadable == 1 );
}

TEST_CASE( "an absent session directory audits as empty, not as an error", "[eval][audit]" ) {
	const auto audit = eval::audit_sessions( scratch( "audit-missing" ) / "nope" );

	REQUIRE( audit.has_value( ) );
	REQUIRE( audit->runs == 0 );
	REQUIRE( audit->false_success_rate( ) == 0.0 );
}
