#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <string>
#include <vector>

#include "mcode/agent/repetition_guard.hxx"

using namespace mcode::agent;

namespace {

	// A realistic thinking block: it advances, so its tail is not periodic.
	[[nodiscard]] auto ordinary_thinking( ) -> std::string {
		return "I need to look at the config loader first. It returns bool and swallows "
			"the error code, so callers cannot tell which failure they got. Let me read "
			"the file to see how the error is constructed, then check the three call "
			"sites that need the new return type before I change the signature.";
	}

}

TEST_CASE( "a repeated tail is a loop, ordinary prose is not", "[agent][guard]" ) {
	SECTION( "exact contiguous repetition fires" ) {
		const auto unit = std::string{ "let me check the git status before continuing. " };

		CHECK( periodic_tail_length( unit + unit + unit, REPETITION_MIN_UNIT,
			REPETITION_MIN_COPIES ) > 0 );
	}

	SECTION( "advancing prose does not fire" ) {
		// The property that matters: legitimate long thinking must never trip it.
		const auto thinking = ordinary_thinking( );

		CHECK( periodic_tail_length( thinking, REPETITION_MIN_UNIT,
			REPETITION_MIN_COPIES ) == 0 );
		CHECK_FALSE( detect_repetition( thinking, { } ).looped );
	}

	SECTION( "a tail shorter than the floor does not fire" ) {
		// Below `REPETITION_MIN_UNIT * REPETITION_MIN_COPIES` there is not
		// enough text to be periodic at the smallest allowed unit.
		const auto short_tail = std::string{ "abcabcabcabcabcabcabcabcabcabcabcabcabcabc" };

		CHECK( short_tail.size( ) < REPETITION_MIN_UNIT * REPETITION_MIN_COPIES );
		CHECK( periodic_tail_length( short_tail, REPETITION_MIN_UNIT,
			REPETITION_MIN_COPIES ) == 0 );
	}

	SECTION( "a genuinely repetitive string fires even at a short period" ) {
		// "abc" repeated is periodic with period 3, so it is also periodic with
		// period 30 -- the scan starts at the floor, not at 3, and still finds
		// it. That is correct: no real message is 120 characters of "abc".
		auto short_period = std::string{ };

		for ( auto index = 0; index < 40; ++index ) {
			short_period += "abc";
		}

		CHECK( periodic_tail_length( short_period, REPETITION_MIN_UNIT,
			REPETITION_MIN_COPIES ) > 0 );
		CHECK( detect_repetition( short_period, { } ).looped );
	}

	SECTION( "a single copy is not a loop" ) {
		const auto once = std::string{ "this sentence appears exactly one time here" };

		CHECK( periodic_tail_length( once, REPETITION_MIN_UNIT,
			REPETITION_MIN_COPIES ) == 0 );
	}
}

TEST_CASE( "the cross-turn signal catches a reworded restatement", "[agent][guard]" ) {
	const auto first =
		"Let me read the config loader to see how it reports a failure. It returns bool "
		"and swallows the error code, so the callers cannot distinguish the cases. I will "
		"read the file and then look at the call sites before changing anything at all.";

	// A near-duplicate: same content, minor rewording. Not exactly periodic, so
	// only the cross-turn signal can catch it.
	const auto second =
		"Let me read the config loader to see how it reports a failure. It returns bool "
		"and swallows the error code, so the callers cannot distinguish the cases. I will "
		"read the file and then look at the call sites before changing anything much.";

	const auto verdict = detect_repetition( second, { first } );

	CHECK( verdict.looped );
	CHECK( verdict.repeated_characters > 0 );
}

TEST_CASE( "the cross-turn signal ignores short and distinct turns", "[agent][guard]" ) {
	SECTION( "a short turn is never compared" ) {
		CHECK_FALSE( detect_repetition( "Done.", { "Done." } ).looped );
	}

	SECTION( "distinct turns are not a loop" ) {
		const auto first = ordinary_thinking( );
		const auto second = std::string{
			"The build succeeded. Now I will run the test suite to confirm the change "
			"did not break the three call sites I just updated, and then commit." };

		CHECK_FALSE( detect_repetition( second, { first } ).looped );
	}
}

// The cross-turn signal compares two normalized strings. Comparing a normalized
// one against a raw one made it dead: a verbatim repeat that differed only in
// whitespace and case measured 0.885 against a 0.90 threshold and never fired.
TEST_CASE( "a repeat that differs only in spacing and case still fires",
	"[agent][guard]" ) {
	const auto first = ordinary_thinking( );
	auto second = std::string{ };

	for ( const auto character : first ) {
		// Same words, different whitespace and case.
		second.push_back( character == ' ' ? '\n' : static_cast< char >( std::toupper(
			static_cast< unsigned char >( character ) ) ) );
	}

	CHECK( detect_repetition( second, { first } ).looped );
}

TEST_CASE( "normalization collapses whitespace and case only", "[agent][guard]" ) {
	CHECK( normalize_for_comparison( "  Hello   World  " ) == "hello world" );
	CHECK( normalize_for_comparison( "a\nb\tc" ) == "a b c" );

	// Whitespace is collapsed, never dropped, so these stay distinct.
	CHECK( normalize_for_comparison( "a b" ) != normalize_for_comparison( "ab" ) );
}

TEST_CASE( "shingle overlap is a proportion in range", "[agent][guard]" ) {
	CHECK( shingle_overlap( "", "" ) == 0.0 );
	CHECK( shingle_overlap( "abc", "abcdefgh" ) == 0.0 );

	const auto text = std::string{ "the quick brown fox jumps over the lazy dog" };

	CHECK( shingle_overlap( text, text ) == 1.0 );
	CHECK( shingle_overlap( text, "zzzzzzzzzzzzzzzzzzzzzzzzzzzz" ) == 0.0 );
}
