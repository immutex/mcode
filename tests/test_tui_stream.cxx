#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/transcript.hxx"
#include "mcode/tui/tty.hxx"

#include "tui_test_helpers.hxx"

using namespace mcode::tui;

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

TEST_CASE( "a tall reasoning block does not push the answer out of the region",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::thinking_delta;

	for ( auto index = std::size_t{ 0 }; index < 40; ++index ) {
		delta.text = "thought " + std::to_string( index ) + "\n";
		coordinator.apply( delta );
	}

	delta.type = event_queue::kind::assistant_delta;

	for ( auto index = std::size_t{ 0 }; index < 40; ++index ) {
		delta.text = "answer " + std::to_string( index ) + "\n";
		coordinator.apply( delta );
	}

	const auto bytes = coordinator.flush( );

	// the reasoning is bounded live, so the answer keeps rows instead of being dropped
	CHECK( bytes.find( "answer 39" ) != std::string::npos );
	CHECK( bytes.find( "thought 39" ) != std::string::npos );
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

	const auto frame = build_frame( painted, tui_test::ROWS, tui_test::COLUMNS, 1 );

	CHECK( painted.streaming_rows.size( ) == 1 );
	CHECK( painted.stream_render_count == 1 );
	CHECK( frame.rows( ) == tui_test::ROWS );
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
		REQUIRE( tui_test::same_rows( coordinator.state( ).streaming_rows,
			transcript::render_block( text, token::text ) ) );
	}

	CHECK( coordinator.state( ).streaming_text == text );
}
