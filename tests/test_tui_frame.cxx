// The frame builder, the diff, and the emitter, asserted on bytes.

#include <catch2/catch_test_macros.hpp>

#include <cctype>
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
	const auto bytes = emitter.emit( previous, current );

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

TEST_CASE( "the first frame is drawn, not swallowed", "[tui][render]" ) {
	// The prompt has to reach the terminal before the first key is read. The
	// frame builder was only ever reached from a bus event, so an idle session
	// showed nothing at all -- no prompt, no status line -- until something was
	// typed, which reads as a program that hung.
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
	// A blank cell has no text. Appending its `text` wrote nothing, so the
	// previous frame's glyph stayed on screen -- every backspace, and every
	// line that got shorter, left ghost characters behind.
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	coordinator.set_prompt( "hello", 5 );
	const auto typed = coordinator.flush( );
	REQUIRE( typed.find( "hello" ) != std::string::npos );

	// Backspace: one cell shorter, and the removed column must be repainted.
	coordinator.set_prompt( "hell", 4 );
	const auto erased = coordinator.flush( );

	CHECK_FALSE( erased.empty( ) );

	// Only the changed cell is re-emitted, so the visible text of this frame is
	// the erase itself: a single space where the 'o' was. Emitting nothing
	// would leave the 'o' on screen.
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
	// The frame is painted with absolute addressing and leaves the cursor at
	// the end of the last changed run, which is not where the user is typing.
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );
	coordinator.set_prompt( "hi", 2 );

	const auto bytes = coordinator.flush( );

	// The prompt is the last row of the live region, and the caret sits after
	// "> hi" -- four columns in, which is column 5 in the 1-based CHA the
	// terminal speaks. The row needs no movement: the emitter parks the cursor
	// on that row, which is what makes the addressing relative rather than
	// pinned to a fixed screen row.
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

	// Two bytes, one column: a caret placed by byte count would sit one cell
	// too far right.
	coordinator.set_prompt( "\u00e9", 2 );

	CHECK( coordinator.state( ).input_cursor == 1 );
}

TEST_CASE( "every escape the emitter writes is well formed", "[tui][render]" ) {
	// A sequence that closed early left its parameters to be printed as text:
	// the frame carried "\x1b[0m" followed by ";90;37m", so the terminal
	// executed the reset and then drew the literal characters ";90;37m" on
	// screen. Parsing the output back is the only assertion that catches it --
	// the bytes are "valid" ANSI, just in the wrong place.
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
			// A `;` here is a parameter that escaped its sequence and is about
			// to be drawn as text -- the exact shape of the bug. Inside a
			// sequence it is legal, which is why this is checked only outside.
			CHECK( bytes[ cursor ] != ';' );
			++cursor;

			continue;
		}

		++sequences;
		REQUIRE( cursor + 1 < bytes.size( ) );
		REQUIRE( bytes[ cursor + 1 ] == '[' );

		cursor += 2;

		// Only digits, ';' and '?' may appear before the final byte, which must
		// be a letter. A parameter after the terminator is the bug.
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
	// Two defects on one row. `elapsed_ms` and `spinner_frame` were written
	// once at `tool_start` and never advanced, so a running call showed a
	// frozen spinner and `0.0s` for its whole life; and the verb came from a
	// `tool_call` payload that had already been moved into the event log, so
	// the row read `\u280b   0.0s` with no tool name at all.
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

	// The producer's stamp is the call's start, so four seconds later the row
	// must read 4.0s rather than 0.0s.
	coordinator.advance_tools( 5'000 );

	CHECK( coordinator.state( ).tools.size( ) == 1 );
	CHECK( coordinator.state( ).tools.front( ).verb == "bash" );
	CHECK( coordinator.state( ).tools.front( ).elapsed_ms == 4'000 );

	const auto first_frame = coordinator.state( ).tools.front( ).spinner_frame;

	coordinator.advance_tools( 5'000 + SPINNER_INTERVAL_MS * 3 );

	CHECK( coordinator.state( ).tools.front( ).elapsed_ms == 4'000 + SPINNER_INTERVAL_MS * 3 );
	CHECK( coordinator.state( ).tools.front( ).spinner_frame != first_frame );

	// A stamp of zero means the producer supplied none, which leaves the row
	// at its initial value rather than inventing a start time.
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
	// Tool results were collected into a member nothing rendered, so the
	// transcript showed the spinner and then nothing at all.
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

TEST_CASE( "a committed answer is erased from the region and printed",
	"[tui][render]" ) {
	// The commit must erase the region before printing, or the text lands on
	// the rows the next frame repaints and is overwritten.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 100 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "Red";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "\x1b[0J" ) != std::string::npos );
	CHECK( bytes.find( "Red" ) != std::string::npos );
}

TEST_CASE( "a turn's answer survives the turn ending", "[tui][render]" ) {
	// `turn_end` clears the live region. Discarding the accumulated text there
	// erased the reply before it could be read, so the transcript only ever
	// showed tool activity.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = "the answer";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "the answer" ) != std::string::npos );
}
