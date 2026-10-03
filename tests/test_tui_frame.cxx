#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

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

TEST_CASE( "a committed answer is erased from the region and printed",
	"[tui][render]" ) {
	// the region must be erased before printing, or the next frame repaints over the text.
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

TEST_CASE( "a chain of thought commits as one line", "[tui][render]" ) {
	// the live row shows only the tail, so committing the whole text would
	// dump dozens of lines into scrollback per turn.
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::thinking_delta;
	delta.text = "first thought\nsecond thought\nthird thought";
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "first thought" ) != std::string::npos );
	CHECK( bytes.find( "second thought" ) == std::string::npos );
	CHECK( bytes.find( "third thought" ) == std::string::npos );
}

TEST_CASE( "a long chain of thought is bounded to the terminal width",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::thinking_delta;
	delta.text = std::string( 200, 'x' );
	coordinator.apply( delta );

	auto end = event_queue::item{ };
	end.type = event_queue::kind::turn_end;
	coordinator.apply( end );

	const auto bytes = coordinator.flush( );

	// No committed line may exceed the terminal width.
	auto longest = std::size_t{ 0 };
	auto run = std::size_t{ 0 };

	for ( const auto character : bytes ) {
		run = character == 'x' ? run + 1 : 0;
		longest = std::max( longest, run );
	}

	CHECK( longest > 0 );
	CHECK( longest < 40 );
}

TEST_CASE( "a turn's answer survives the turn ending", "[tui][render]" ) {
	// turn_end clears the live region, so the answer must be committed before it.
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
