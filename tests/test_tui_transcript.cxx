#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/transcript.hxx"

using namespace mcode::tui;

namespace {

	[[nodiscard]] auto row_width( const styled_line& row ) -> std::size_t {
		auto total = std::size_t{ 0 };

		for ( const auto& span : row ) {
			total += string_width( span.text, 1 );
		}

		return total;
	}

	[[nodiscard]] auto row_text( const styled_line& row ) -> std::string {
		auto text = std::string{ };

		for ( const auto& span : row ) {
			text += span.text;
		}

		return text;
	}

	[[nodiscard]] auto row_bold( const styled_line& row ) -> bool {
		for ( const auto& span : row ) {
			if ( span.bold ) {
				return true;
			}
		}

		return false;
	}

}

TEST_CASE( "a committed row wraps at word boundaries within the column budget",
	"[tui][transcript]" ) {
	auto text = std::string{ };

	for ( auto index = std::size_t{ 0 }; index < 40; ++index ) {
		text += "word ";
	}

	const auto source = transcript::render_block( text, token::text );

	REQUIRE( source.size( ) == 1 );
	REQUIRE( row_width( source.front( ) ) > 40 );

	const auto wrapped = transcript::wrap_rows( source, 40, 1 );

	REQUIRE( wrapped.size( ) > 1 );

	for ( const auto& row : wrapped ) {
		CHECK( row_width( row ) <= 40 );

		// every row holds whole words: the split never lands mid-word.
		auto rest = row_text( row );

		while ( !rest.empty( ) ) {
			REQUIRE( rest.starts_with( "word" ) );
			rest.erase( 0, 4 );

			if ( rest.starts_with( " " ) ) {
				rest.erase( 0, 1 );
			}
		}
	}
}

TEST_CASE( "a wrapped bullet hangs its continuation rows under the text",
	"[tui][transcript]" ) {
	const auto text = std::string{ "- " } +
		"the quick brown fox jumps over the lazy dog and keeps on running";

	const auto source = transcript::render_block( text, token::text );

	REQUIRE( source.size( ) == 1 );
	REQUIRE( source.front( ).size( ) == 2 );
	REQUIRE( source.front( ).front( ).text == "• " );
	REQUIRE( row_width( source.front( ) ) > 40 );

	const auto wrapped = transcript::wrap_rows( source, 40, 1 );

	REQUIRE( wrapped.size( ) > 1 );

	// the marker is two columns wide, and each continuation repeats them as
	// spaces, so the text hangs under the bullet's body and not at column 0.
	for ( auto index = std::size_t{ 1 }; index < wrapped.size( ); ++index ) {
		REQUIRE_FALSE( wrapped[ index ].empty( ) );
		CHECK( wrapped[ index ].front( ).text == "  " );
		CHECK( row_width( wrapped[ index ] ) <= 40 );
	}
}

TEST_CASE( "a wrapped blockquote repeats its gutter on continuation rows",
	"[tui][transcript]" ) {
	const auto text = std::string{ "> " } +
		"the quick brown fox jumps over the lazy dog and keeps on running";

	const auto source = transcript::render_block( text, token::text );

	REQUIRE( source.size( ) == 1 );
	REQUIRE( source.front( ).front( ).text == "│ " );

	const auto wrapped = transcript::wrap_rows( source, 40, 1 );

	REQUIRE( wrapped.size( ) > 1 );

	for ( auto index = std::size_t{ 1 }; index < wrapped.size( ); ++index ) {
		REQUIRE_FALSE( wrapped[ index ].empty( ) );
		CHECK( wrapped[ index ].front( ).text == "│ " );
	}
}

TEST_CASE( "a bold span straddling the wrap point stays bold on both rows",
	"[tui][transcript]" ) {
	const auto row = styled_line{
		{ "start ", token::text },
		{ "the bold run crosses the wrap column here", token::text, token::none, true },
	};

	REQUIRE( row_width( row ) > 24 );

	const auto wrapped = transcript::wrap_row( row, 24, 1 );

	REQUIRE( wrapped.size( ) >= 2 );
	CHECK_FALSE( wrapped.front( ).front( ).bold );

	for ( const auto& line : wrapped ) {
		CHECK( row_bold( line ) );
		CHECK( row_width( line ) <= 24 );
	}
}

TEST_CASE( "a single word longer than the row splits on cluster boundaries",
	"[tui][transcript]" ) {
	const auto word = std::string( 100, 'w' );
	const auto row = styled_line{ { word, token::text } };

	const auto wrapped = transcript::wrap_row( row, 30, 1 );

	REQUIRE( wrapped.size( ) > 1 );

	auto joined = std::string{ };

	for ( const auto& line : wrapped ) {
		CHECK( row_width( line ) <= 30 );
		joined += row_text( line );
	}

	// nothing is dropped and nothing overflows.
	CHECK( joined == word );
}

TEST_CASE( "a fenced code row is passed through unmodified", "[tui][transcript]" ) {
	const auto row = styled_line{ { std::string( 80, 'x' ), token::text, token::code_bg } };

	const auto wrapped = transcript::wrap_row( row, 20, 1 );

	REQUIRE( wrapped.size( ) == 1 );
	REQUIRE( wrapped.front( ).size( ) == 1 );
	CHECK( wrapped.front( ).front( ).text == row.front( ).text );
	CHECK( wrapped.front( ).front( ).background == token::code_bg );
	CHECK( row_width( wrapped.front( ) ) == 80 );
}
