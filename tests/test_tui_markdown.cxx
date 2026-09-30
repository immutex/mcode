// The incremental markdown parser: block boundaries, streaming, and the
// closed-blocks-are-immutable rule.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mcode/tui/markdown.hxx"

using namespace mcode::tui;

TEST_CASE( "a heading is its own block and closes immediately", "[tui][markdown]" ) {
	auto parser = markdown_parser{ };

	const auto changed = parser.feed( "# Title\n" );

	REQUIRE( changed.size( ) == 1 );
	REQUIRE( parser.blocks( ).size( ) == 1 );

	const auto& block = parser.blocks( ).front( );
	CHECK( block.type == md_block::kind::heading );
	CHECK( block.level == 1 );
	CHECK_FALSE( block.open );
	REQUIRE( block.lines.size( ) == 1 );
	CHECK( block.lines.front( ).front( ).text == "Title" );
	CHECK( block.lines.front( ).front( ).bold );
}

TEST_CASE( "a paragraph absorbs consecutive lines and closes on a blank line",
	"[tui][markdown]" ) {
	auto parser = markdown_parser{ };

	std::ignore = parser.feed( "first line\n" );
	std::ignore = parser.feed( "second line\n" );

	REQUIRE( parser.blocks( ).size( ) == 1 );
	CHECK( parser.blocks( ).front( ).open );
	CHECK( parser.blocks( ).front( ).lines.size( ) == 2 );

	std::ignore = parser.feed( "\n" );

	REQUIRE( parser.blocks( ).size( ) == 1 );
	CHECK_FALSE( parser.blocks( ).front( ).open );
}

TEST_CASE( "a fenced code block opens, holds, and closes on the second fence",
	"[tui][markdown]" ) {
	auto parser = markdown_parser{ };

	std::ignore = parser.feed( "```cxx\n" );
	REQUIRE( parser.blocks( ).size( ) == 1 );
	CHECK( parser.blocks( ).front( ).type == md_block::kind::fenced_code );
	CHECK( parser.blocks( ).front( ).info == "cxx" );
	CHECK( parser.blocks( ).front( ).open );

	std::ignore = parser.feed( "int main( ) { }\n" );
	REQUIRE( parser.blocks( ).size( ) == 1 );
	REQUIRE( parser.blocks( ).front( ).lines.size( ) == 1 );
	CHECK( parser.blocks( ).front( ).lines.front( ).front( ).background == token::code_bg );

	std::ignore = parser.feed( "```\n" );
	CHECK_FALSE( parser.blocks( ).front( ).open );
}

TEST_CASE( "only the last open block re-renders per token batch", "[tui][markdown]" ) {
	auto parser = markdown_parser{ };

	std::ignore = parser.feed( "# Done\n\n" );
	const auto committed_count = parser.blocks( ).size( );
	REQUIRE( committed_count == 1 );
	CHECK_FALSE( parser.blocks( ).front( ).open );

	const auto changed = parser.feed( "streaming text\n" );

	// The closed heading block is not in the changed set.
	REQUIRE( changed.size( ) == 1 );
	CHECK( changed.front( ).type == md_block::kind::paragraph );
	CHECK( changed.front( ).open );
}

TEST_CASE( "a partial line buffers until its newline arrives", "[tui][markdown]" ) {
	auto parser = markdown_parser{ };

	std::ignore = parser.feed( "hel" );
	CHECK( parser.blocks( ).empty( ) );

	std::ignore = parser.feed( "lo\n" );
	REQUIRE( parser.blocks( ).size( ) == 1 );
	CHECK( parser.blocks( ).front( ).lines.front( ).front( ).text == "hello" );
}

TEST_CASE( "finish closes the trailing open block", "[tui][markdown]" ) {
	auto parser = markdown_parser{ };

	std::ignore = parser.feed( "tail without a newline" );

	const auto changed = parser.finish( );
	REQUIRE( changed.size( ) == 1 );
	REQUIRE( parser.blocks( ).size( ) == 1 );
	CHECK_FALSE( parser.blocks( ).front( ).open );
	CHECK( parser.blocks( ).front( ).lines.front( ).front( ).text ==
		"tail without a newline" );
}

TEST_CASE( "list items group into one block", "[tui][markdown]" ) {
	auto parser = markdown_parser{ };

	std::ignore = parser.feed( "- one\n" );
	std::ignore = parser.feed( "- two\n" );

	REQUIRE( parser.blocks( ).size( ) == 1 );
	CHECK( parser.blocks( ).front( ).type == md_block::kind::list_item );
	CHECK( parser.blocks( ).front( ).lines.size( ) == 2 );
}

TEST_CASE( "inline formatting splits bold, italic and code", "[tui][markdown]" ) {
	const auto spans = render_inline( "a **bold** and *soft* and `code`", token::text );

	REQUIRE( spans.size( ) == 6 );
	CHECK( spans[ 0 ].text == "a " );
	CHECK_FALSE( spans[ 0 ].bold );
	CHECK( spans[ 1 ].text == "bold" );
	CHECK( spans[ 1 ].bold );
	CHECK( spans[ 2 ].text == " and " );
	CHECK( spans[ 3 ].text == "soft" );
	CHECK( spans[ 3 ].italic );
	CHECK( spans[ 4 ].text == " and " );
	CHECK( spans[ 5 ].text == "code" );
	CHECK( spans[ 5 ].dim );
}

TEST_CASE( "an unterminated marker stays literal", "[tui][markdown]" ) {
	const auto spans = render_inline( "a * b", token::text );

	REQUIRE( spans.size( ) == 1 );
	CHECK( spans.front( ).text == "a * b" );
}
