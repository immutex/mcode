#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/transcript.hxx"
#include "mcode/tui/tty.hxx"

using namespace mcode::tui;

namespace {

	// U+2502 BOX DRAWINGS LIGHT VERTICAL: the transcript's output gutter, and
	// the East Asian Ambiguous glyph the terminal may render two columns wide.
	constexpr std::string_view BOX_VERTICAL = "\xE2\x94\x82";

	[[nodiscard]] auto row_width( const styled_line& row, const std::size_t ambiguous_width )
		-> std::size_t {
		auto total = std::size_t{ 0 };

		for ( const auto& span : row ) {
			total += string_width( span.text, ambiguous_width );
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

}

TEST_CASE( "the ambiguous width is probed as exactly two or one", "[tui][width]" ) {
	CHECK( probe_capabilities( "", "", false, true, "2" ).ambiguous_width == 2 );

	// Unset, and every value that is not "2", is the one-column default: an
	// unknown value is not an error and is not logged.
	CHECK( probe_capabilities( "", "", false, true ).ambiguous_width == 1 );
	CHECK( probe_capabilities( "", "", false, true, "" ).ambiguous_width == 1 );
	CHECK( probe_capabilities( "", "", false, true, "1" ).ambiguous_width == 1 );
	CHECK( probe_capabilities( "", "", false, true, "3" ).ambiguous_width == 1 );
	CHECK( probe_capabilities( "", "", false, true, "x" ).ambiguous_width == 1 );

	// The struct's own default is the unset case, so a capability set built
	// without a probe renders byte-identically to today's output.
	CHECK( capabilities{ }.ambiguous_width == 1 );
}

TEST_CASE( "an ambiguous code point takes the policy's column count", "[tui][width]" ) {
	CHECK( codepoint_width( 0x2502, 1 ) == 1 );
	CHECK( codepoint_width( 0x2502, 2 ) == 2 );

	// The other chrome glyphs the design names, and the horizontal rule.
	CHECK( codepoint_width( 0x2500, 1 ) == 1 );
	CHECK( codepoint_width( 0x2500, 2 ) == 2 );
	CHECK( codepoint_width( 0x2022, 2 ) == 2 );
	CHECK( codepoint_width( 0x2026, 2 ) == 2 );

	// A wide code point and a combining mark are unaffected by the policy.
	CHECK( codepoint_width( 0x4E16, 1 ) == 2 );
	CHECK( codepoint_width( 0x4E16, 2 ) == 2 );
	CHECK( codepoint_width( 0x0301, 2 ) == 0 );
}

TEST_CASE( "a row containing an ambiguous glyph measures one column wider",
	"[tui][width]" ) {
	const auto row = std::string{ "a" } + std::string{ BOX_VERTICAL } + "b";

	CHECK( string_width( row, 1 ) == 3 );
	CHECK( string_width( row, 2 ) == 4 );

	// The delta is exactly the glyph's extra column: nothing else moves.
	CHECK( string_width( row, 2 ) - string_width( row, 1 ) == 1 );
}

TEST_CASE( "truncation respects the ambiguous width", "[tui][width]" ) {
	const auto text = std::string{ "ab" } + std::string{ BOX_VERTICAL } + "cd";

	// At one column the glyph fits inside a three-column budget; at two it
	// does not, so the truncation stops before it.
	CHECK( truncate_to_width( text, 3, 1 ) ==
		std::string{ "ab" } + std::string{ BOX_VERTICAL } );
	CHECK( truncate_to_width( text, 3, 2 ) == "ab" );

	// A truncation never splits the glyph, whichever width it is given.
	CHECK( string_width( truncate_to_width( text, 4, 2 ), 2 ) == 4 );
}

TEST_CASE( "a buffer lays an ambiguous glyph out at the policy's width",
	"[tui][width]" ) {
	const auto bar = std::string{ BOX_VERTICAL };

	auto narrow = cell_buffer{ 1, 8, 1 };
	auto wide = cell_buffer{ 1, 8, 2 };

	REQUIRE( narrow.write_text( 0, 0, bar, style{ } ) == 1 );
	REQUIRE( wide.write_text( 0, 0, bar, style{ } ) == 2 );

	CHECK( narrow.at( 0, 0 ).width == 1 );
	CHECK( wide.at( 0, 0 ).width == 2 );

	// The wide buffer marks the second cell as the continuation slot, which
	// the emitter skips rather than painting twice.
	CHECK( wide.at( 0, 1 ).width == 0 );
	CHECK( wide.ambiguous_width( ) == 2 );

	// The constructor's default is the unset environment's one column.
	CHECK( cell_buffer{ 1, 8 }.ambiguous_width( ) == 1 );
}

TEST_CASE( "wrapping respects the ambiguous width", "[tui][width]" ) {
	// Twelve gutters and a word: sixteen columns at policy 1, which fits, and
	// twenty-eight at policy 2, which does not.
	auto text = std::string{ };

	for ( auto index = std::size_t{ 0 }; index < 12; ++index ) {
		text += BOX_VERTICAL;
	}

	text += " end";

	const auto source = transcript::render_block( text, token::text );
	REQUIRE( source.size( ) == 1 );
	REQUIRE( row_width( source.front( ), 1 ) == 16 );

	const auto narrow = transcript::wrap_rows( source, 20, 1 );
	const auto wide = transcript::wrap_rows( source, 20, 2 );

	CHECK( narrow.size( ) == 1 );
	CHECK( wide.size( ) > 1 );

	// The policy changes where the break lands, never what is printed.
	auto rejoined = std::string{ };

	for ( const auto& row : wide ) {
		rejoined += row_text( row );
	}

	CHECK( rejoined.find( std::string{ BOX_VERTICAL } ) != std::string::npos );
	CHECK( rejoined.ends_with( "end" ) );
}

TEST_CASE( "a cluster write outside the buffer is refused", "[tui][width]" ) {
	auto buffer = cell_buffer{ 1, 4, 1 };

	// a row past the last must not write through a wrapped index
	CHECK( buffer.set_cluster( 1, 0, "x", 1, style{ } ) == 0 );
	CHECK( buffer.set_cluster( 4, 0, "x", 1, style{ } ) == 0 );

	// the in-range write still lands.
	CHECK( buffer.set_cluster( 0, 0, "x", 1, style{ } ) == 1 );
	CHECK( buffer.at( 0, 0 ).text == "x" );
}

TEST_CASE( "a marker wider than the row keeps its own row", "[tui][width]" ) {
	// two columns hold the bullet or the content, never both, so the bullet takes its own row
	const auto source = transcript::render_block( "- item", token::text );
	REQUIRE( source.size( ) == 1 );

	const auto wrapped = transcript::wrap_rows( source, 2, 1 );

	REQUIRE( wrapped.size( ) > 1 );
	CHECK( row_text( wrapped.front( ) ) == "• " );

	auto rejoined = std::string{ };

	for ( const auto& row : wrapped ) {
		rejoined += row_text( row );
	}

	CHECK( rejoined == "• item" );
}
