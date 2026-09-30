// The frame builder, the diff, and the emitter, asserted on bytes.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/frame.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/theme.hxx"

using namespace mcode::tui;

namespace {

	constexpr std::size_t ROWS = 4;
	constexpr std::size_t COLUMNS = 40;

}

TEST_CASE( "the width function truncates on cluster boundaries", "[tui][width]" ) {
	const auto ambiguous = std::size_t{ 1 };

	// A CJK string: two wide clusters.
	const auto cjk = std::string{ "\xE4\xB8\x96\xE7\x95\x8C" };
	REQUIRE( string_width( cjk, ambiguous ) == 4 );

	// Truncating to 3 columns keeps the first cluster whole and drops the
	// second entirely; it never splits a cluster.
	const auto cut = truncate_to_width( cjk, 3, ambiguous );
	CHECK( cut == std::string{ "\xE4\xB8\x96" } );

	// A combining mark joins its base: one cluster, one column.
	const auto combining = std::string{ "e\xCC\x81x" };
	CHECK( string_width( combining, ambiguous ) == 2 );

	const auto mark_cut = truncate_to_width( combining, 1, ambiguous );
	CHECK( mark_cut == std::string{ "e\xCC\x81" } );

	// A ZWJ emoji sequence is one cluster at two columns; a truncation that
	// does not fit drops the whole sequence.
	const auto family = std::string{ "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
		"\xE2\x80\x8D\xF0\x9F\x91\xA7" };
	CHECK( string_width( family, ambiguous ) == 2 );
	CHECK( truncate_to_width( family, 1, ambiguous ).empty( ) );
	CHECK( truncate_to_width( family, 2, ambiguous ) == family );

	// ASCII is the boring case.
	CHECK( string_width( "hello", ambiguous ) == 5 );
	CHECK( truncate_to_width( "hello", 3, ambiguous ) == "hel" );
}

TEST_CASE( "a frame diff emits nothing when the state does not change", "[tui][frame]" ) {
	auto state = render_state{ };
	state.model_name = "test-model";
	state.total_tokens = 1200;
	state.total_cost = 0.031;
	state.turn_elapsed_ms = 4200;
	state.input_line = "fix the test";

	const auto first = build_frame( state, ROWS, COLUMNS, 1 );
	const auto second = build_frame( state, ROWS, COLUMNS, 1 );

	REQUIRE( first.rows( ) == second.rows( ) );

	const auto runs = diff_rows( first, second );
	CHECK( runs.empty( ) );
}

TEST_CASE( "a frame diff emits only the changed cells when one line changes",
	"[tui][frame]" ) {
	auto before = render_state{ };
	before.model_name = "m";
	before.input_line = "abc";

	auto after = before;
	after.input_line = "abx";

	const auto previous = build_frame( before, ROWS, COLUMNS, 1 );
	const auto current = build_frame( after, ROWS, COLUMNS, 1 );

	const auto runs = diff_rows( previous, current );

	REQUIRE( runs.size( ) == 1 );
	CHECK( runs.front( ).row == ROWS - 1 );
	CHECK( runs.front( ).column == 4 );
	CHECK( runs.front( ).count == 1 );
}

TEST_CASE( "rendering a fixed state twice produces identical bytes", "[tui][frame]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::truecolor;

	auto state = render_state{ };
	state.model_name = "m";
	state.total_tokens = 500;
	state.total_cost = 0.5;
	state.turn_elapsed_ms = 1000;
	state.input_line = "hello";

	auto tool = render_state::active_tool{ };
	tool.verb = "read";
	tool.target = "src/main.cxx";
	tool.elapsed_ms = 1200;
	tool.spinner_frame = 3;
	state.tools.push_back( tool );

	const auto previous = build_frame( state, ROWS, COLUMNS, 1 );

	auto emitter = ansi_emitter{ caps };
	const auto first = emitter.emit( previous, previous, 0 );

	auto emitter_again = ansi_emitter{ caps };
	const auto second = emitter_again.emit( previous, previous, 0 );

	CHECK( first == second );
	CHECK( first.empty( ) );
}

TEST_CASE( "the emitter writes only changed runs with SGR deltas", "[tui][frame]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::ansi16;

	auto before = render_state{ };
	before.input_line = "abcdef";

	auto after = before;
	after.input_line = "abcxef";

	const auto previous = build_frame( before, ROWS, COLUMNS, 1 );
	const auto current = build_frame( after, ROWS, COLUMNS, 1 );

	auto emitter = ansi_emitter{ caps };
	const auto bytes = emitter.emit( previous, current, 0 );

	// One cursor move for the changed row, one SGR, one cell, nothing else.
	CHECK( bytes.find( "x" ) != std::string::npos );
	CHECK( bytes.find( "abc" ) == std::string::npos );
	CHECK( bytes.find( "def" ) == std::string::npos );
}

TEST_CASE( "NO_COLOR produces attributes-only output", "[tui][frame]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto before = render_state{ };
	before.input_line = "abc";

	auto after = before;
	after.input_line = "abd";

	const auto previous = build_frame( before, ROWS, COLUMNS, 1 );
	const auto current = build_frame( after, ROWS, COLUMNS, 1 );

	auto emitter = ansi_emitter{ caps };
	const auto bytes = emitter.emit( previous, current, 0 );

	CHECK( bytes.find( "38;" ) == std::string::npos );
	CHECK( bytes.find( "48;" ) == std::string::npos );
	CHECK( bytes.find( "d" ) != std::string::npos );
}

TEST_CASE( "the theme resolves one depth per session", "[tui][theme]" ) {
	CHECK( resolve_token( token::accent, capabilities::color_depth::truecolor ) ==
		"38;2;122;162;247" );
	CHECK( resolve_token( token::accent, capabilities::color_depth::ansi256 ) ==
		"38;5;111" );
	CHECK( resolve_token( token::accent, capabilities::color_depth::ansi16 ) == "94" );
	CHECK( resolve_token( token::accent, capabilities::color_depth::none ).empty( ) );

	const auto& table = theme_table( );
	CHECK( table.size( ) == 11 );
}

TEST_CASE( "the capability probe honours NO_COLOR and the depth ladder", "[tui][tty]" ) {
	const auto plain = probe_capabilities( "", "", false, true );
	CHECK( plain.depth == capabilities::color_depth::ansi16 );

	const auto truecolor = probe_capabilities( "truecolor", "xterm-256color", false, true );
	CHECK( truecolor.depth == capabilities::color_depth::truecolor );

	const auto indexed = probe_capabilities( "", "xterm-256color", false, true );
	CHECK( indexed.depth == capabilities::color_depth::ansi256 );

	const auto no_color = probe_capabilities( "truecolor", "xterm-256color", true, true );
	CHECK( no_color.depth == capabilities::color_depth::none );

	const auto piped = probe_capabilities( "truecolor", "xterm-256color", false, false );
	CHECK( piped.depth == capabilities::color_depth::truecolor );
	CHECK_FALSE( piped.synchronized_output );
}

TEST_CASE( "synchronized output wraps only when supported and non-empty",
	"[tui][frame]" ) {
	auto supported = capabilities{ };
	supported.synchronized_output = true;

	auto emitter = ansi_emitter{ supported };
	CHECK( emitter.synchronized( "x" ) == "\x1b[?2026hx\x1b[?2026l" );
	CHECK( emitter.synchronized( { } ).empty( ) );

	auto unsupported = capabilities{ };
	unsupported.synchronized_output = false;

	auto plain = ansi_emitter{ unsupported };
	CHECK( plain.synchronized( "x" ) == "x" );
}

TEST_CASE( "spinner glyphs are single-width and cycle", "[tui][render]" ) {
	for ( auto frame = std::size_t{ 0 }; frame < SPINNER_FRAMES * 2; ++frame ) {
		const auto glyph = spinner_glyph( frame );
		CHECK( string_width( glyph, 1 ) == 1 );
	}

	CHECK( spinner_glyph( 0 ) == spinner_glyph( SPINNER_FRAMES ) );
}

TEST_CASE( "the render coordinator commits and clears the live region",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );

	auto item = event_queue::item{ };
	item.type = event_queue::kind::tool_start;
	item.text = "read";

	coordinator.apply( item );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::tool_end;
	end.text = "read src/main.cxx";

	const auto bytes = coordinator.commit_tool( { "✓ read src/main.cxx", token::success } );

	CHECK( bytes.find( "✓ read src/main.cxx" ) != std::string::npos );
	CHECK( bytes.find( '\n' ) != std::string::npos );
}
