#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mcode/support/text.hxx"

using namespace mcode::text;

namespace {

	constexpr std::string_view CJK = "\xE4\xB8\x96\xE7\x95\x8C";
	constexpr std::string_view EMOJI = "\xF0\x9F\x98\x80x";

}

TEST_CASE( "UTF-8 validation", "[text]" ) {
	CHECK( is_valid_utf8( "" ) );
	CHECK( is_valid_utf8( "plain ascii" ) );
	CHECK( is_valid_utf8( CJK ) );
	CHECK( is_valid_utf8( EMOJI ) );

	CHECK_FALSE( is_valid_utf8( "\xFF" ) );
	CHECK_FALSE( is_valid_utf8( "\x80" ) );
	CHECK_FALSE( is_valid_utf8( "\xC0\x80" ) );
	CHECK_FALSE( is_valid_utf8( "\xED\xA0\x80" ) );
}

TEST_CASE( "code point counting is not byte counting", "[text]" ) {
	CHECK( codepoint_count( "" ) == 0 );
	CHECK( codepoint_count( "abc" ) == 3 );
	CHECK( codepoint_count( CJK ) == 2 );
	CHECK( codepoint_count( EMOJI ) == 2 );
	CHECK( CJK.size( ) == 6 );
}

TEST_CASE( "truncation lands on code-point boundaries", "[text]" ) {
	SECTION( "a cut inside a multi-byte sequence backs up" ) {
		const auto offset = truncate_offset( CJK, 4 );
		CHECK( offset == 3 );
		CHECK( is_valid_utf8( std::string{ CJK }.substr( 0, offset ) ) );
	}

	SECTION( "an exact boundary is kept" ) {
		CHECK( truncate_offset( CJK, 3 ) == 3 );
		CHECK( truncate_offset( CJK, 6 ) == 6 );
	}

	SECTION( "a limit smaller than one code point yields nothing" ) {
		CHECK( truncate_offset( CJK, 1 ) == 0 );
	}

	SECTION( "a 4-byte code point is not split" ) {
		CHECK( truncate_offset( EMOJI, 2 ) == 0 );
		CHECK( truncate_offset( EMOJI, 4 ) == 4 );
	}

	SECTION( "ASCII truncates exactly" ) {
		CHECK( truncate_offset( "abcdef", 3 ) == 3 );
	}
}

TEST_CASE( "truncate appends the ellipsis only when it cut", "[text]" ) {
	CHECK( truncate( "abcdef", 6 ) == "abcdef" );
	CHECK( truncate( "abcdef", 3 ) == "abc..." );
	CHECK( truncate( CJK, 4 ) == std::string{ CJK }.substr( 0, 3 ) + "..." );

	for ( auto limit = std::size_t{ 0 }; limit <= CJK.size( ) + 1; ++limit ) {
		CHECK( is_valid_utf8( truncate( CJK, limit ) ) );
	}
}

TEST_CASE( "sanitize replaces invalid sequences and preserves valid ones", "[text]" ) {
	CHECK( sanitize_utf8( "clean" ) == "clean" );
	CHECK( sanitize_utf8( CJK ) == std::string{ CJK } );

	const auto cleaned = sanitize_utf8( "a\xFFz" );
	CHECK( is_valid_utf8( cleaned ) );
	CHECK( cleaned.front( ) == 'a' );
	CHECK( cleaned.back( ) == 'z' );
	CHECK( cleaned.find( "\xEF\xBF\xBD" ) != std::string::npos );

	CHECK( sanitize_utf8( EMOJI ) == std::string{ EMOJI } );
}

TEST_CASE( "UTF-16 round trip", "[text]" ) {
	SECTION( "ASCII" ) {
		auto wide = to_utf16( "hello" );
		REQUIRE( wide );
		CHECK( wide->size( ) == 5 );

		auto back = from_utf16( *wide );
		REQUIRE( back );
		CHECK( *back == "hello" );
	}

	SECTION( "CJK" ) {
		auto wide = to_utf16( CJK );
		REQUIRE( wide );
		CHECK( wide->size( ) == 2 );

		auto back = from_utf16( *wide );
		REQUIRE( back );
		CHECK( *back == std::string{ CJK } );
	}

	SECTION( "a code point above the BMP becomes a surrogate pair" ) {
		auto wide = to_utf16( EMOJI );
		REQUIRE( wide );
		CHECK( wide->size( ) == 3 );
	}

	SECTION( "invalid input is refused rather than transcoded" ) {
		auto wide = to_utf16( "\xFF" );
		REQUIRE_FALSE( wide );
		CHECK( wide.error( ).code == mcode::errc::unsupported );
	}

	SECTION( "empty input round-trips" ) {
		auto wide = to_utf16( "" );
		REQUIRE( wide );
		CHECK( wide->empty( ) );

		auto back = from_utf16( *wide );
		REQUIRE( back );
		CHECK( back->empty( ) );
	}
}
