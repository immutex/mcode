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
	// every line but the last must reach the frame, not just the tail of the buffer.
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

	CHECK( region_rows_for( coordinator.state( ), 30 ) == 5 );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "first line" ) != std::string::npos );
	CHECK( bytes.find( "third line" ) != std::string::npos );

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

	// A line at a time, the way the provider streams: re-rendering the whole buffer
	// per delta is quadratic in the answer's length.
	const auto lines = std::size_t{ 500 };

	for ( auto index = std::size_t{ 0 }; index < lines; ++index ) {
		delta.text = "line " + std::to_string( index ) + "\n";
		coordinator.apply( delta );
	}

	CHECK( coordinator.stream_render_count( ) == 0 );

	const auto bytes = coordinator.flush( );

	// one paint, one whole-buffer render, whatever the delta count.
	CHECK( coordinator.stream_render_count( ) == 1 );

	REQUIRE( coordinator.state( ).streaming_rows.size( ) == lines );
	CHECK( bytes.find( "line 499" ) != std::string::npos );

	const auto idle = coordinator.flush( );

	CHECK( idle == std::string{ "\x1b[3G" } );
	CHECK( coordinator.stream_render_count( ) == 1 );

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

	CHECK( painted.find( "thought 99" ) != std::string::npos );
}

TEST_CASE( "sizing a state materialises the block it holds", "[tui][render]" ) {
	// The region's height is read from the rows, so the sizing path has to bring them
	// up to date: a height taken from stale rows lags the text by a frame.
	auto state = render_state{ };
	state.streaming_text = "one\ntwo";
	state.streaming_stale = true;

	REQUIRE( state.streaming_rows.empty( ) );

	CHECK( region_rows_for( state, 30 ) == 4 );
	CHECK( state.streaming_rows.size( ) == 2 );
	CHECK( state.stream_render_count == 1 );

	CHECK( region_rows_for( state, 30 ) == 4 );
	CHECK( state.stream_render_count == 1 );

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
	// no paint happened between the deltas and the call, so the commit has to render
	// the buffer it holds.
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

	CHECK( coordinator.state( ).streaming_text.empty( ) );
	CHECK( coordinator.state( ).streaming_rows.empty( ) );
}

TEST_CASE( "a thought closes with the reasoning streamed so far",
	"[tui][render]" ) {
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
	// The pump's shape: drain a tick's deltas, then paint once, and every paint shows
	// the whole buffer streamed so far.
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

		const auto painted = coordinator.flush( );

		REQUIRE( coordinator.stream_render_count( ) == tick + 1 );
		REQUIRE_FALSE( painted.empty( ) );
		REQUIRE( tui_test::same_rows( coordinator.state( ).streaming_rows,
			transcript::render_block( text, token::text ) ) );
	}

	CHECK( coordinator.state( ).streaming_text == text );
}
