#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mcode/tui/markdown.hxx"

using namespace mcode::tui;

TEST_CASE( "inline formatting splits bold, italic and code", "[tui][markdown]" ) {
	const auto spans = render_inline( "a **b** c *d* `e`", token::text );

	auto text = std::string{ };
	auto saw_bold = false;
	auto saw_italic = false;
	auto saw_code = false;

	for ( const auto& span : spans ) {
		text += span.text;

		saw_bold = saw_bold || span.bold;
		saw_italic = saw_italic || span.italic;
		saw_code = saw_code || span.color == token::warn;
	}

	CHECK( text == "a b c d e" );
	CHECK( saw_bold );
	CHECK( saw_italic );
	CHECK( saw_code );
}

TEST_CASE( "an unterminated marker stays literal", "[tui][markdown]" ) {
	const auto spans = render_inline( "a * b", token::text );

	REQUIRE( spans.size( ) == 1 );
	CHECK( spans.front( ).text == "a * b" );
}
