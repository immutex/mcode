#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/transcript.hxx"
#include "mcode/tui/tty.hxx"

using namespace mcode::tui;

namespace {

	constexpr std::size_t ROWS = 4;
	constexpr std::size_t COLUMNS = 40;

	[[nodiscard]] auto row_text( const styled_line& row ) -> std::string {
		auto text = std::string{ };

		for ( const auto& span : row ) {
			text += span.text;
		}

		return text;
	}

	// A state's rendered rows against a fresh render of the same buffer: a
	// cache that lags shows up as a text difference, which is what this
	// compares.
	[[nodiscard]] auto same_rows( const std::vector< styled_line >& left,
		const std::vector< styled_line >& right ) -> bool {
		if ( left.size( ) != right.size( ) ) {
			return false;
		}

		for ( auto index = std::size_t{ 0 }; index < left.size( ); ++index ) {
			if ( row_text( left[ index ] ) != row_text( right[ index ] ) ) {
				return false;
			}
		}

		return true;
	}

}

TEST_CASE( "spinner glyphs are single-width and cycle", "[tui][render]" ) {
	for ( auto frame = std::size_t{ 0 }; frame < SPINNER_FRAMES * 2; ++frame ) {
		const auto glyph = spinner_glyph( frame );
		CHECK( string_width( glyph, 1 ) == 1 );
	}

	CHECK( spinner_glyph( 0 ) == spinner_glyph( SPINNER_FRAMES ) );
}

TEST_CASE( "the first frame is drawn, not swallowed", "[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );

	coordinator.resize( 24, 40 );
	coordinator.set_prompt( "fix the test", 12 );

	const auto bytes = coordinator.flush( );

	CHECK_FALSE( bytes.empty( ) );
	CHECK( bytes.find( "fix the test" ) != std::string::npos );
	CHECK( bytes.find( "> " ) != std::string::npos );
}

TEST_CASE( "an erased cell reaches the terminal as a space", "[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	coordinator.set_prompt( "hello", 5 );
	const auto typed = coordinator.flush( );
	REQUIRE( typed.find( "hello" ) != std::string::npos );

	coordinator.set_prompt( "hell", 4 );
	const auto erased = coordinator.flush( );

	CHECK_FALSE( erased.empty( ) );

	// the removed column must be repainted, or the old glyph stays on screen.
	auto visible = std::string{ };

	for ( std::size_t index = 0; index < erased.size( ); ++index ) {
		if ( erased[ index ] == '\x1b' ) {
			while ( index < erased.size( ) &&
				!std::isalpha( static_cast< unsigned char >( erased[ index ] ) ) ) {
				++index;
			}

			continue;
		}

		visible.push_back( erased[ index ] );
	}

	CHECK( visible == " " );
}

TEST_CASE( "the caret lands on the prompt row, under the typed text",
	"[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );
	coordinator.set_prompt( "hi", 2 );

	const auto bytes = coordinator.flush( );

	// the caret column is 1-based CHA.
	const auto expected = std::string{ "\x1b[5G" };

	CHECK( bytes.ends_with( expected ) );
}

TEST_CASE( "the caret column is measured in display cells, not bytes",
	"[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	// two bytes, one column: a byte-count caret would sit one cell too far right.
	coordinator.set_prompt( "\u00e9", 2 );

	CHECK( coordinator.state( ).input_cursor == 1 );
}

TEST_CASE( "the caret follows the probed ambiguous width", "[tui][render]" ) {
	// U+2502, the output gutter: one column under the default policy, two
	// under the East Asian one. The frame builder and the caret read the same
	// probed value, so a row containing it cannot desynchronise the grid.
	const auto gutter = std::string{ "\xE2\x94\x82" };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto narrow = render_coordinator{ };
	narrow.set_capabilities( caps );
	narrow.resize( 24, 40 );
	narrow.set_prompt( gutter, gutter.size( ) );

	auto wide_caps = caps;
	wide_caps.ambiguous_width = 2;

	auto wide = render_coordinator{ };
	wide.set_capabilities( wide_caps );
	wide.resize( 24, 40 );
	wide.set_prompt( gutter, gutter.size( ) );

	// the gutter is one display column under one, two under two.
	CHECK( narrow.state( ).input_cursor == 1 );
	CHECK( wide.state( ).input_cursor == 2 );

	// The caret is the prompt prefix plus the measured text, so it sits one
	// column further right under the two-column policy. A terminal painting
	// the glyph two cells wide puts the cursor exactly there.
	CHECK( narrow.flush( ).ends_with( "\x1b[4G" ) );
	CHECK( wide.flush( ).ends_with( "\x1b[5G" ) );
}

TEST_CASE( "every escape the emitter writes is well formed", "[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::ansi16;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );
	coordinator.set_prompt( "hello", 5 );

	const auto bytes = coordinator.flush( );
	REQUIRE_FALSE( bytes.empty( ) );

	auto cursor = std::size_t{ 0 };
	auto sequences = std::size_t{ 0 };

	while ( cursor < bytes.size( ) ) {
		if ( bytes[ cursor ] != '\x1b' ) {
			CHECK( bytes[ cursor ] != ';' );
			++cursor;

			continue;
		}

		++sequences;
		REQUIRE( cursor + 1 < bytes.size( ) );
		REQUIRE( bytes[ cursor + 1 ] == '[' );

		cursor += 2;

		// only digits, ';' and '?' may appear before the final letter byte.
		while ( cursor < bytes.size( ) && !std::isalpha(
			static_cast< unsigned char >( bytes[ cursor ] ) ) ) {
			const auto character = bytes[ cursor ];
			const auto is_parameter = ( character >= '0' && character <= '9' ) ||
				character == ';' || character == '?';

			REQUIRE( is_parameter );

			++cursor;
		}

		REQUIRE( cursor < bytes.size( ) );
		++cursor;
	}

	CHECK( sequences > 0 );
}

TEST_CASE( "an active tool row names the tool and runs its clock", "[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto start = event_queue::item{ };
	start.type = event_queue::kind::tool_start;
	start.text = "bash";
	start.stamp_ms = 1'000;
	coordinator.apply( start );

	coordinator.advance_tools( 5'000 );

	CHECK( coordinator.state( ).tools.size( ) == 1 );
	CHECK( coordinator.state( ).tools.front( ).verb == "bash" );
	CHECK( coordinator.state( ).tools.front( ).elapsed_ms == 4'000 );

	const auto first_frame = coordinator.state( ).tools.front( ).spinner_frame;

	coordinator.advance_tools( 5'000 + SPINNER_INTERVAL_MS * 3 );

	CHECK( coordinator.state( ).tools.front( ).elapsed_ms == 4'000 + SPINNER_INTERVAL_MS * 3 );
	CHECK( coordinator.state( ).tools.front( ).spinner_frame != first_frame );

	// a stamp of zero means no start time was supplied, so the row keeps its initial value.
	auto unstamped = render_coordinator{ };
	unstamped.set_capabilities( caps );
	unstamped.resize( 24, 40 );

	auto unknown = event_queue::item{ };
	unknown.type = event_queue::kind::tool_start;
	unknown.text = "read";
	unstamped.apply( unknown );
	unstamped.advance_tools( 9'999'999 );

	CHECK( unstamped.state( ).tools.front( ).elapsed_ms == 0 );
}

TEST_CASE( "a finished tool call is written to scrollback", "[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto start = event_queue::item{ };
	start.type = event_queue::kind::tool_start;
	start.text = "read";
	coordinator.apply( start );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::tool_end;
	end.text = "read src/main.cxx";
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "read src/main.cxx" ) != std::string::npos );
	CHECK( bytes.find( '\n' ) != std::string::npos );
}

TEST_CASE( "a streamed answer shows every line, formatted, while it streams",
	"[tui][render]" ) {
	// only the tail of the buffer used to reach the frame, so every line but
	// the last stayed invisible until the turn ended.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "first line\nsecond **line**\nthird line";
	coordinator.apply( delta );

	REQUIRE( coordinator.state( ).streaming_rows.size( ) == 3 );

	// the region grows with the block: prompt + status + three rows.
	CHECK( region_rows_for( coordinator.state( ), 30 ) == 5 );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "first line" ) != std::string::npos );
	CHECK( bytes.find( "third line" ) != std::string::npos );

	// the bold span carries its own SGR, so the markers never reach the frame.
	CHECK( bytes.find( "second " ) != std::string::npos );
	CHECK( bytes.find( "line" ) != std::string::npos );
	CHECK( bytes.find( "**" ) == std::string::npos );
}

TEST_CASE( "a streamed block taller than the region keeps its newest rows",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto text = std::string{ };

	for ( auto index = std::size_t{ 0 }; index <= 40; ++index ) {
		text += "row" + std::to_string( index ) + "\n";
	}

	text += "row41";

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = text;
	coordinator.apply( delta );

	REQUIRE( coordinator.state( ).streaming_rows.size( ) == 42 );
	CHECK( region_rows_for( coordinator.state( ), 30 ) == LIVE_REGION_MAX_ROWS );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "row41" ) != std::string::npos );
	CHECK( bytes.find( "row0" ) == std::string::npos );
}

TEST_CASE( "a burst of deltas is rendered once per paint, not once per delta",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;

	// A line at a time, the way the provider streams. Re-rendering the whole
	// buffer per delta is quadratic in the answer's length, and that cost is
	// what made a long answer feel slow.
	const auto lines = std::size_t{ 500 };

	for ( auto index = std::size_t{ 0 }; index < lines; ++index ) {
		delta.text = "line " + std::to_string( index ) + "\n";
		coordinator.apply( delta );
	}

	// the deltas only appended: nothing has re-rendered the buffer.
	CHECK( coordinator.stream_render_count( ) == 0 );

	const auto bytes = coordinator.flush( );

	// one paint, one whole-buffer render, whatever the delta count.
	CHECK( coordinator.stream_render_count( ) == 1 );

	// and the rows that paint built are the whole buffer, not the last delta.
	REQUIRE( coordinator.state( ).streaming_rows.size( ) == lines );
	CHECK( bytes.find( "line 499" ) != std::string::npos );

	// a paint with nothing new renders nothing at all: the caret is the only
	// thing it writes.
	const auto idle = coordinator.flush( );

	CHECK( idle == std::string{ "\x1b[3G" } );
	CHECK( coordinator.stream_render_count( ) == 1 );

	// the reasoning block is bounded the same way.
	delta.type = event_queue::kind::thinking_delta;

	const auto thoughts = std::size_t{ 100 };

	for ( auto index = std::size_t{ 0 }; index < thoughts; ++index ) {
		delta.text = "thought " + std::to_string( index ) + "\n";
		coordinator.apply( delta );
	}

	CHECK( coordinator.stream_render_count( ) == 1 );

	const auto painted = coordinator.flush( );

	CHECK( coordinator.stream_render_count( ) == 2 );
	CHECK( coordinator.state( ).thinking_rows.size( ) == thoughts );

	// the region keeps the newest rows, so the newest thought is on screen.
	CHECK( painted.find( "thought 99" ) != std::string::npos );
}

TEST_CASE( "sizing a state materialises the block it holds", "[tui][render]" ) {
	// the region's height is read from the rows, so the sizing path is one of
	// the readers that has to bring them up to date: a height taken from
	// stale rows lags the text by a frame.
	auto state = render_state{ };
	state.streaming_text = "one\ntwo";
	state.streaming_stale = true;

	REQUIRE( state.streaming_rows.empty( ) );

	// prompt + status + two rows.
	CHECK( region_rows_for( state, 30 ) == 4 );
	CHECK( state.streaming_rows.size( ) == 2 );
	CHECK( state.stream_render_count == 1 );

	// a second sizing is free: the rows already match the buffer.
	CHECK( region_rows_for( state, 30 ) == 4 );
	CHECK( state.stream_render_count == 1 );

	// the frame builder is the same reader, so a stale state still paints its
	// buffer: it is the second caller of the one render path, not a second
	// render path.
	auto painted = render_state{ };
	painted.streaming_text = "stale but current";
	painted.streaming_stale = true;

	const auto frame = build_frame( painted, ROWS, COLUMNS, 1 );

	CHECK( painted.streaming_rows.size( ) == 1 );
	CHECK( painted.stream_render_count == 1 );
	CHECK( frame.rows( ) == ROWS );
}

TEST_CASE( "a tool call closes the block with the text streamed so far",
	"[tui][render]" ) {
	// no paint happened between the deltas and the call, so the commit has to
	// render the buffer it holds: committing a row cache the deltas left
	// behind would drop the prose.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "prose before the call\nsecond row of it";
	coordinator.apply( delta );

	auto start = event_queue::item{ };
	start.type = event_queue::kind::tool_start;
	start.text = "bash";
	coordinator.apply( start );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "prose before the call" ) != std::string::npos );
	CHECK( bytes.find( "second row of it" ) != std::string::npos );

	// and the closed block is gone from the live region.
	CHECK( coordinator.state( ).streaming_text.empty( ) );
	CHECK( coordinator.state( ).streaming_rows.empty( ) );
}

TEST_CASE( "a thought closes with the reasoning streamed so far",
	"[tui][render]" ) {
	// the thought block commits from its rendered rows, and the deltas left
	// them stale: the commit path is what materialises them.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::thinking_delta;
	delta.text = "weighed the first option\nweighed the second";
	coordinator.apply( delta );

	auto start = event_queue::item{ };
	start.type = event_queue::kind::tool_start;
	start.text = "read";
	coordinator.apply( start );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "weighed the first option" ) != std::string::npos );
	CHECK( bytes.find( "weighed the second" ) != std::string::npos );
}

TEST_CASE( "a render happens once per paint, whatever the delta rate",
	"[tui][render]" ) {
	// The pump's real shape: drain a tick's deltas, then paint once. The
	// bound is renders == paints, and every paint has to show the whole
	// buffer streamed so far -- not the tick's slice of it.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;

	const auto ticks = std::size_t{ 25 };
	const auto per_tick = std::size_t{ 20 };
	auto text = std::string{ };

	for ( auto tick = std::size_t{ 0 }; tick < ticks; ++tick ) {
		for ( auto index = std::size_t{ 0 }; index < per_tick; ++index ) {
			const auto piece = "word" + std::to_string( tick ) + " ";
			delta.text = piece;
			coordinator.apply( delta );
			text += piece;
		}

		// one render per paint, and the rows are the whole buffer.
		const auto painted = coordinator.flush( );

		REQUIRE( coordinator.stream_render_count( ) == tick + 1 );
		REQUIRE_FALSE( painted.empty( ) );
		REQUIRE( same_rows( coordinator.state( ).streaming_rows,
			transcript::render_block( text, token::text ) ) );
	}

	CHECK( coordinator.state( ).streaming_text == text );
}
