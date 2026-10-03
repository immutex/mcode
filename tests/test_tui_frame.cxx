#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/frame.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/theme.hxx"
#include "mcode/tui/tty.hxx"

using namespace mcode::tui;

namespace {

	constexpr std::size_t ROWS = 4;
	constexpr std::size_t COLUMNS = 40;

}

TEST_CASE( "the width function truncates on cluster boundaries", "[tui][width]" ) {
	const auto ambiguous = std::size_t{ 1 };

	const auto cjk = std::string{ "\xE4\xB8\x96\xE7\x95\x8C" };
	REQUIRE( string_width( cjk, ambiguous ) == 4 );

	// a truncation never splits a grapheme cluster.
	const auto cut = truncate_to_width( cjk, 3, ambiguous );
	CHECK( cut == std::string{ "\xE4\xB8\x96" } );

	const auto combining = std::string{ "e\xCC\x81x" };
	CHECK( string_width( combining, ambiguous ) == 2 );

	const auto mark_cut = truncate_to_width( combining, 1, ambiguous );
	CHECK( mark_cut == std::string{ "e\xCC\x81" } );

	// a truncation that does not fit drops the whole ZWJ sequence.
	const auto family = std::string{ "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
		"\xE2\x80\x8D\xF0\x9F\x91\xA7" };
	CHECK( string_width( family, ambiguous ) == 2 );
	CHECK( truncate_to_width( family, 1, ambiguous ).empty( ) );
	CHECK( truncate_to_width( family, 2, ambiguous ) == family );

	CHECK( string_width( "hello", ambiguous ) == 5 );
	CHECK( truncate_to_width( "hello", 3, ambiguous ) == "hel" );
}

TEST_CASE( "a frame at the two-column policy lays an ambiguous glyph two cells wide",
	"[tui][frame]" ) {
	// The frame's own geometry: the typed gutter must occupy two cells at
	// policy 2 and one at policy 1, so the text after it starts a column
	// later. The caret is measured from the same width, so this is what keeps
	// the two in step.
	const auto bar = std::string{ "\xE2\x94\x82" };

	auto state = render_state{ };
	state.input_line = bar + "x";

	const auto narrow = build_frame( state, ROWS, COLUMNS, 1 );
	const auto wide = build_frame( state, ROWS, COLUMNS, 2 );

	const auto row = ROWS - 1;
	const auto prefix = std::size_t{ 2 };

	REQUIRE( narrow.at( row, prefix ).text == bar );
	CHECK( narrow.at( row, prefix ).width == 1 );
	REQUIRE( wide.at( row, prefix ).text == bar );
	CHECK( wide.at( row, prefix ).width == 2 );

	// At policy 1 the `x` is the next cell; at policy 2 the next cell is the
	// bar's continuation slot and the `x` follows it.
	CHECK( narrow.at( row, prefix + 1 ).text == "x" );
	CHECK( wide.at( row, prefix + 1 ).width == 0 );
	CHECK( wide.at( row, prefix + 2 ).text == "x" );
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
	const auto first = emitter.emit( previous, previous );

	auto emitter_again = ansi_emitter{ caps };
	const auto second = emitter_again.emit( previous, previous );

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
	const auto bytes = emitter.emit( previous, current );

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
	const auto bytes = emitter.emit( previous, current );

	CHECK( bytes.find( "38;" ) == std::string::npos );
	CHECK( bytes.find( "48;" ) == std::string::npos );
	CHECK( bytes.find( "d" ) != std::string::npos );
}

TEST_CASE( "the theme resolves one depth per session", "[tui][theme]" ) {
	CHECK( token_color( token::accent, capabilities::color_depth::truecolor ) ==
		"38;2;122;162;247" );
	CHECK( token_color( token::accent, capabilities::color_depth::ansi256 ) == "38;5;111" );
	CHECK( token_color( token::accent, capabilities::color_depth::ansi16 ) == "94" );
	CHECK( token_color( token::accent, capabilities::color_depth::none ).empty( ) );

	// `none` is the absent sentinel and must resolve to no colour at all; a
	// value here would be emitted after the real one and win.
	CHECK( token_color( token::none, capabilities::color_depth::truecolor ).empty( ) );
	CHECK( token_color( token::none, capabilities::color_depth::ansi16 ).empty( ) );

	// A background token is stored foreground-shaped, so it must be rewritten
	// into the 48 family; reading it through token_color would emit a second
	// foreground and the code block would render as near-black text.
	CHECK( token_background( token::code_bg, capabilities::color_depth::truecolor ) ==
		"48;2;26;27;38" );
	CHECK( token_background( token::code_bg, capabilities::color_depth::ansi256 ) ==
		"48;5;234" );
	CHECK( token_background( token::none, capabilities::color_depth::truecolor ).empty( ) );

	for ( const auto& entry : theme_table( ) ) {
		if ( entry.name == token::none ) {
			continue;
		}

		CHECK_FALSE( token_color( entry.name, capabilities::color_depth::truecolor ).empty( ) );
		CHECK_FALSE( std::string_view{ entry.truecolor }.empty( ) );
	}
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
