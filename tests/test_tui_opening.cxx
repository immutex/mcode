#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/tui/opening.hxx"
#include "mcode/tui/render.hxx"

#include "tui_test_helpers.hxx"

using namespace mcode;
using namespace mcode::tui;

namespace {

	inline constexpr std::size_t SCREEN_ROWS = 30;
	inline constexpr std::size_t SCREEN_COLUMNS = 100;

	// The opening sequence as a real terminal would end up showing it. The
	// coordinator emits bytes; only a screen that has a cursor can say whether
	// the banner survived them.
	[[nodiscard]] auto opened_screen( const bool permissive )
		-> tui_test::screen_model {
		auto coordinator = render_coordinator{ };

		auto caps = capabilities{ };
		caps.depth = capabilities::color_depth::none;
		coordinator.set_capabilities( caps );
		coordinator.resize( SCREEN_ROWS, SCREEN_COLUMNS );

		auto opening = session_opening{ };
		opening.version = "0.0.1";
		opening.platform = "windows-AMD64";
		opening.compiler = "msvc-19.51.36260.0";
		opening.permissive = permissive;

		auto screen = tui_test::screen_model{ SCREEN_ROWS, SCREEN_COLUMNS };
		screen.feed( emit_opening( coordinator, opening ) );

		return screen;
	}

}

// The defect this exists for: `emit_opening` wrote a `flush` and then a
// `reserve` that emitted `screen_rows - 1` newlines. A flush leaves the cursor
// on the LAST row, so every newline scrolled -- the banner was committed and
// then pushed off the top in the same breath. It rendered as an empty screen
// with the prompt on row 0 and the meter on row 28, while `visible_text` over
// the same bytes still contained every banner string, so all 118 existing tui
// assertions passed.
TEST_CASE( "the opening banner survives the bytes that paint it", "[tui][opening]" ) {
	const auto screen = opened_screen( true );

	CHECK( screen.find_row( "mcode 0.0.1" ) < SCREEN_ROWS );
	CHECK( screen.find_row( "windows-AMD64" ) < SCREEN_ROWS );
	CHECK( screen.find_row( "msvc-19.51.36260.0" ) < SCREEN_ROWS );
	CHECK( screen.find_row( "/help for commands" ) < SCREEN_ROWS );

	// The platform and compiler are one line: a split would mean a wrap.
	CHECK( screen.find_row( "windows-AMD64, msvc-19.51.36260.0" ) < SCREEN_ROWS );
}

TEST_CASE( "the opening leaves the prompt on the last row under its meter",
	"[tui][opening]" ) {
	const auto screen = opened_screen( true );

	CHECK( screen.row( SCREEN_ROWS - 1 ) == ">" );
	CHECK( screen.row( SCREEN_ROWS - 2 ).find( "0 tok" ) != std::string::npos );
	CHECK( screen.row( SCREEN_ROWS - 2 ).find( "$0.000" ) != std::string::npos );
}

// A permissive session states its boundary; a prompting one must not claim the
// boundary is off, which is the failure the notice exists to prevent.
TEST_CASE( "the approval boundary is stated only when it applies", "[tui][opening]" ) {
	const auto permissive = opened_screen( true );

	CHECK( permissive.find_row( "edits and commands run without prompting" ) < SCREEN_ROWS );
	CHECK( permissive.find_row( "the hard-deny floor" ) < SCREEN_ROWS );

	const auto prompting = opened_screen( false );

	CHECK( prompting.find_row( "edits and commands run without prompting" ) == SCREEN_ROWS );
	CHECK( prompting.find_row( "the hard-deny floor" ) == SCREEN_ROWS );
}

// The banner is above the region, and the layout above it is tight: the scroll
// count is not itself the invariant, because `commit` scrolls on purpose to
// clear its output -- the invariant is that the banner ends up where the layout
// puts it, with no residue of blank rows between the banner and the region.
TEST_CASE( "the banner sits above the live region with no residue", "[tui][opening]" ) {
	const auto screen = opened_screen( true );

	const auto banner = screen.find_row( "mcode 0.0.1" );
	const auto hint = screen.find_row( "/help for commands" );
	const auto meter = screen.find_row( "0 tok" );

	REQUIRE( banner < SCREEN_ROWS );
	REQUIRE( hint < SCREEN_ROWS );
	REQUIRE( meter < SCREEN_ROWS );
	CHECK( banner < hint );
	CHECK( hint < meter );

	// Every row from the last banner line to the meter belongs to the layout.
	// The banner ends with one deliberate blank line (its trailing separator),
	// so one blank is the layout and more than that is residue. A stale
	// `painted_rows_` used to erase and scroll rows the region never covered,
	// which showed up here as four extra blank rows.
	auto blank = std::size_t{ 0 };

	for ( auto index = hint + 1; index < meter; ++index ) {
		if ( screen.row( index ).empty( ) ) {
			++blank;
		}
	}

	CHECK( blank <= 2 );
}
