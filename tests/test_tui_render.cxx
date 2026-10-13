#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstddef>
#include <string>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"
#include "tui_test_helpers.hxx"

using namespace mcode::tui;

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

	// the removed column must be repainted, or the stale glyph stays on screen.
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

TEST_CASE( "invalidate repaints every column of the region", "[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	coordinator.set_prompt( "hello", 5 );
	CHECK_FALSE( coordinator.flush( ).empty( ) );

	const auto blank_row = std::string{ PROMPT_PREFIX } +
		std::string( tui_test::COLUMNS - 1 - PROMPT_PREFIX_WIDTH, ' ' );

	auto plain = render_coordinator{ };
	plain.set_capabilities( caps );
	plain.resize( 24, 40 );
	plain.set_prompt( "hello", 5 );
	CHECK_FALSE( plain.flush( ).empty( ) );
	plain.set_prompt( "", 0 );

	CHECK( tui_test::visible_text( plain.flush( ) ).find( blank_row ) == std::string::npos );

	coordinator.set_prompt( "", 0 );
	coordinator.invalidate( );

	CHECK( tui_test::visible_text( coordinator.flush( ) ).find( blank_row ) != std::string::npos );
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
	// U+2502 is one column under the default policy and two under the East Asian one;
	// the frame builder and the caret read the same probed value.
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

	CHECK( narrow.state( ).input_cursor == 1 );
	CHECK( wide.state( ).input_cursor == 2 );

	// The caret is the prompt prefix plus the measured text, so a terminal painting the
	// glyph two cells wide puts the cursor exactly there.
	CHECK( narrow.flush( ).ends_with( "\x1b[4G" ) );
	CHECK( wide.flush( ).ends_with( "\x1b[5G" ) );
}

TEST_CASE( "ghost text is spliced only at the end of the input", "[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 24, 40 );

	auto palette = slash_palette{ };
	palette.open = true;
	palette.query = "/he";
	palette.matches.push_back( slash_command{ "/help", "show help" } );
	coordinator.set_palette( palette );

	coordinator.set_prompt( "/he", 3 );

	CHECK( tui_test::visible_text( coordinator.flush( ) ).find( "> /help" ) != std::string::npos );

	coordinator.set_prompt( "/he", 2 );
	coordinator.invalidate( );

	const auto mid_line = tui_test::visible_text( coordinator.flush( ) );

	CHECK( mid_line.find( "/hlpe" ) == std::string::npos );
	CHECK( mid_line.find( "> /he" ) != std::string::npos );
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

		// a CSI parameter is digits, ';' and '?'; the final letter byte ends it.
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

TEST_CASE( "a palette longer than the window scrolls instead of losing its first rows",
	"[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );
	coordinator.resize( 40, 60 );

	auto palette = slash_palette{ };
	palette.open = true;
	palette.query = "/";

	// Names carry no slash: the palette's prefix supplies it.
	for ( auto index = 1; index <= 30; ++index ) {
		auto name = std::string{ "cmd" };

		if ( index < 10 ) {
			name += '0';
		}

		name += std::to_string( index );

		palette.matches.push_back( slash_command{ name, "description" } );
	}

	// The command rows carry a two-column marker, so the prompt row is excluded by
	// its own prefix rather than by position.
	const auto listed = []( const std::vector< std::string >& rows ) {
		auto out = std::vector< std::string >{ };

		for ( const auto& row : rows ) {
			if ( row.starts_with( "> " ) ) {
				continue;
			}

			const auto at = row.find( "/cmd" );

			if ( at == std::string::npos ) {
				continue;
			}

			out.push_back( row.substr( at, 6 ) );
		}

		return out;
	};

	coordinator.set_palette( palette );
	coordinator.set_prompt( "/", 1 );

	auto screen = tui_test::screen_model{ 40, 60 };
	screen.feed( coordinator.flush( ) );

	const auto top = listed( screen.rows( ) );

	REQUIRE( top.size( ) == PALETTE_MAX_ROWS );
	CHECK( top.front( ) == "/cmd01" );
	CHECK( top.back( ) == "/cmd10" );

	// Rows hidden below are announced on the last visible row, and the selected row
	// keeps its own marker.
	CHECK( screen.find_row( "▼ /cmd10" ) < 40 );
	CHECK( screen.find_row( "▸ /cmd01" ) < 40 );
	CHECK( screen.find_row( "▲" ) == 40 );

	// Scrolled to the end: the last command is visible and the first is not.
	palette.selected = palette.matches.size( ) - 1;
	coordinator.set_palette( palette );
	coordinator.invalidate( );

	auto scrolled = tui_test::screen_model{ 40, 60 };
	scrolled.feed( coordinator.flush( ) );

	const auto bottom = listed( scrolled.rows( ) );

	REQUIRE( bottom.size( ) == PALETTE_MAX_ROWS );
	CHECK( bottom.front( ) == "/cmd21" );
	CHECK( bottom.back( ) == "/cmd30" );

	CHECK( scrolled.find_row( "▲ /cmd21" ) < 40 );
	CHECK( scrolled.find_row( "▸ /cmd30" ) < 40 );

	// Nothing is hidden below the tail, so that marker is absent.
	CHECK( scrolled.find_row( "▼" ) == 40 );
}

TEST_CASE( "a short terminal shrinks the palette window instead of shifting it",
	"[tui][render]" ) {
	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;

	auto coordinator = render_coordinator{ };
	coordinator.set_capabilities( caps );

	// A 6-row screen leaves a 5-row region, so the palette gets 3 rows after the
	// prompt and the status line. The window has to start at the first command
	// regardless: the region is painted from the bottom up, and a window that kept
	// its full height here would show the tail of the list.
	coordinator.resize( 6, 40 );

	auto palette = slash_palette{ };
	palette.open = true;
	palette.query = "/";

	for ( auto index = 1; index <= 30; ++index ) {
		auto name = std::string{ "cmd" };

		if ( index < 10 ) {
			name += '0';
		}

		name += std::to_string( index );

		palette.matches.push_back( slash_command{ name, "" } );
	}

	coordinator.set_palette( palette );
	coordinator.set_prompt( "/", 1 );

	auto screen = tui_test::screen_model{ 6, 40 };
	screen.feed( coordinator.flush( ) );

	const auto rows = screen.rows( );

	// The region is the bottom five rows, so row 0 is untouched and the region runs
	// from the status line down to the prompt.
	REQUIRE( rows.size( ) == 6 );
	CHECK( rows[ 0 ].empty( ) );
	CHECK( rows[ 5 ].find( "> /" ) != std::string::npos );

	CHECK( rows[ 2 ].find( "/cmd01" ) != std::string::npos );
	CHECK( rows[ 3 ].find( "/cmd02" ) != std::string::npos );
	CHECK( rows[ 4 ].find( "/cmd03" ) != std::string::npos );

	// The tail is what a full-height window would have shown here.
	CHECK( rows[ 2 ].find( "cmd04" ) == std::string::npos );
	CHECK( rows[ 4 ].find( "cmd28" ) == std::string::npos );
}
