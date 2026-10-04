#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mcode/tui/markdown.hxx"

using namespace mcode::tui;

namespace {

	auto row_text( const styled_line& row ) -> std::string {
		auto text = std::string{ };

		for ( const auto& span : row ) {
			text += span.text;
		}

		return text;
	}

	auto find_span( const styled_line& row, const std::string_view needle ) -> const styled_span* {
		for ( const auto& span : row ) {
			if ( span.text == needle ) {
				return &span;
			}
		}

		return nullptr;
	}

}

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

TEST_CASE( "a heading is bold accent and hides the marker as body text", "[tui][markdown]" ) {
	const auto rows = render_markdown( "# Title", token::text );

	REQUIRE( rows.size( ) == 1 );

	const auto* title = find_span( rows.front( ), "Title" );
	REQUIRE( title != nullptr );
	CHECK( title->color == token::accent );
	CHECK( title->bold );

	auto body = std::string{ };

	for ( const auto& span : rows.front( ) ) {
		if ( !span.dim ) {
			body += span.text;
		}
	}

	CHECK( body == "Title" );
}

TEST_CASE( "an unordered list renders a bullet and one row per item", "[tui][markdown]" ) {
	const auto rows = render_markdown( "- item", token::text );

	REQUIRE( rows.size( ) == 1 );

	const auto* bullet = find_span( rows.front( ), "• " );
	REQUIRE( bullet != nullptr );
	CHECK( bullet->color == token::muted );
	CHECK( find_span( rows.front( ), "item" ) != nullptr );

	const auto many = render_markdown( "- a\n- b", token::text );

	REQUIRE( many.size( ) == 2 );
	CHECK( find_span( many[ 0 ], "a" ) != nullptr );
	CHECK( find_span( many[ 1 ], "b" ) != nullptr );
}

TEST_CASE( "an ordered list keeps its numbers", "[tui][markdown]" ) {
	const auto rows = render_markdown( "1. first\n2. second", token::text );

	REQUIRE( rows.size( ) == 2 );

	const auto first = row_text( rows[ 0 ] );
	const auto second = row_text( rows[ 1 ] );

	CHECK( first.find( "1." ) != std::string::npos );
	CHECK( first.find( "first" ) != std::string::npos );
	CHECK( second.find( "2." ) != std::string::npos );
	CHECK( second.find( "second" ) != std::string::npos );
}

TEST_CASE( "a blockquote renders a muted gutter and dim text", "[tui][markdown]" ) {
	const auto rows = render_markdown( "> quoted", token::text );

	REQUIRE( rows.size( ) == 1 );
	REQUIRE( !rows.front( ).empty( ) );

	const auto& gutter = rows.front( ).front( );
	CHECK( gutter.text.find( "│" ) != std::string::npos );
	CHECK( gutter.color == token::muted );

	const auto* quoted = find_span( rows.front( ), " quoted" );
	REQUIRE( quoted != nullptr );
	CHECK( quoted->dim );
}

TEST_CASE( "a fenced block keeps markers literal on the code background", "[tui][markdown]" ) {
	const auto rows = render_markdown( "```\n**not bold**\n```", token::text );

	REQUIRE( rows.size( ) == 1 );

	const auto* literal = find_span( rows.front( ), "**not bold**" );
	REQUIRE( literal != nullptr );
	CHECK( !literal->bold );
	CHECK( literal->background == token::code_bg );
}

TEST_CASE( "an unterminated fence still renders as code", "[tui][markdown]" ) {
	const auto rows = render_markdown( "```\nstill code", token::text );

	REQUIRE( rows.size( ) == 1 );
	REQUIRE( !rows.front( ).empty( ) );
	CHECK( rows.front( ).front( ).text == "still code" );
	CHECK( rows.front( ).front( ).background == token::code_bg );
}

TEST_CASE( "a horizontal rule becomes a muted rule row", "[tui][markdown]" ) {
	const auto rows = render_markdown( "---", token::text );

	REQUIRE( rows.size( ) == 1 );
	REQUIRE( !rows.front( ).empty( ) );

	const auto rule = row_text( rows.front( ) );

	CHECK( !rule.empty( ) );
	CHECK( rule.find_first_not_of( "─" ) == std::string::npos );
	CHECK( rows.front( ).front( ).color == token::muted );
}

TEST_CASE( "a plain paragraph still runs the inline pass", "[tui][markdown]" ) {
	const auto rows = render_markdown( "**b**", token::text );

	REQUIRE( rows.size( ) == 1 );

	const auto* bold = find_span( rows.front( ), "b" );
	REQUIRE( bold != nullptr );
	CHECK( bold->bold );
}

TEST_CASE( "empty input yields no rows and a trailing newline adds none", "[tui][markdown]" ) {
	CHECK( render_markdown( "", token::text ).empty( ) );

	const auto rows = render_markdown( "a\n", token::text );

	REQUIRE( rows.size( ) == 1 );
	CHECK( row_text( rows.front( ) ) == "a" );
}

TEST_CASE( "a spaced asterisk is arithmetic, not emphasis", "[tui][markdown]" ) {
	// a delimiter needs a non-space inside it, or `2 * 3 * 4` loses both asterisks
	const auto spans = render_inline( "2 * 3 * 4", token::text );

	REQUIRE( spans.size( ) == 1 );
	CHECK( spans.front( ).text == "2 * 3 * 4" );
	CHECK_FALSE( spans.front( ).italic );

	// flanking delimiters still emphasise.
	const auto emphasised = render_inline( "2 *3* 4", token::text );

	auto italic = std::string{ };

	for ( const auto& span : emphasised ) {
		if ( span.italic ) {
			italic += span.text;
		}
	}

	CHECK( italic == "3" );
}
