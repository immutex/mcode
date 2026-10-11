#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"

using namespace mcode::tui;

namespace {

	constexpr std::size_t SCREEN_ROWS = 30;
	constexpr std::size_t SCREEN_COLUMNS = 60;

	constexpr std::size_t HISTORY_REGION = SCREEN_ROWS - 1;
	constexpr std::size_t HISTORY_WINDOW = HISTORY_REGION - 1;

	[[nodiscard]] auto buffer_row_text( const cell_buffer& buffer, const std::size_t row )
		-> std::string {
		auto text = std::string{ };

		for ( auto column = std::size_t{ 0 }; column < buffer.columns( ); ++column ) {
			const auto& value = buffer.at( row, column );

			if ( value.width == 0 ) {
				continue;
			}

			text += value.text.empty( ) ? std::string{ " " } : value.text;
		}

		return text;
	}

	[[nodiscard]] auto buffer_contains( const cell_buffer& buffer,
		const std::string_view needle ) -> bool {
		for ( auto row = std::size_t{ 0 }; row < buffer.rows( ); ++row ) {
			if ( buffer_row_text( buffer, row ).find( needle ) != std::string::npos ) {
				return true;
			}
		}

		return false;
	}

	[[nodiscard]] auto count_occurrences( const std::string_view text,
		const std::string_view needle ) -> std::size_t {
		auto count = std::size_t{ 0 };
		auto at = text.find( needle );

		while ( at != std::string_view::npos ) {
			++count;
			at = text.find( needle, at + needle.size( ) );
		}

		return count;
	}

	// A row without the buffer's trailing padding, so rows compare on text rather
	// than on the layout width.
	[[nodiscard]] auto trimmed( std::string text ) -> std::string {
		while ( !text.empty( ) && text.back( ) == ' ' ) {
			text.pop_back( );
		}

		return text;
	}

	[[nodiscard]] auto make_coordinator( ) -> render_coordinator {
		auto coordinator = render_coordinator{ };

		auto caps = capabilities{ };
		caps.depth = capabilities::color_depth::none;
		coordinator.set_capabilities( caps );
		coordinator.resize( SCREEN_ROWS, SCREEN_COLUMNS );

		return coordinator;
	}

	auto commit_rows( render_coordinator& coordinator, const std::size_t first,
		const std::size_t count ) -> void {
		auto rows = std::vector< styled_line >{ };
		rows.reserve( count );

		for ( auto index = std::size_t{ 0 }; index < count; ++index ) {
			rows.push_back( styled_line{
				{ "row " + std::to_string( first + index ), token::text } } );
		}

		(void)coordinator.commit( std::move( rows ) );
	}

	[[nodiscard]] auto frame_of( const render_coordinator& coordinator,
		const std::size_t screen_rows ) -> cell_buffer {
		const auto region = region_rows_for( coordinator.state( ), screen_rows );

		return build_frame( coordinator.state( ), region, SCREEN_COLUMNS - 1, 1 );
	}

	// The rows the viewport shows, top to bottom, without the indicator row.
	[[nodiscard]] auto viewport_rows( const render_coordinator& coordinator,
		const std::size_t screen_rows ) -> std::vector< std::string > {
		const auto frame = frame_of( coordinator, screen_rows );
		auto rows = std::vector< std::string >{ };

		for ( auto row = std::size_t{ 0 }; row + 1 < frame.rows( ); ++row ) {
			rows.push_back( trimmed( buffer_row_text( frame, row ) ) );
		}

		return rows;
	}

	[[nodiscard]] auto expected_window( const std::size_t first, const std::size_t last )
		-> std::vector< std::string > {
		auto rows = std::vector< std::string >{ };

		for ( auto index = first; index <= last; ++index ) {
			rows.push_back( "row " + std::to_string( index ) );
		}

		return rows;
	}

	[[nodiscard]] auto indicator_visible( const render_coordinator& coordinator,
		const std::size_t screen_rows ) -> bool {
		const auto frame = frame_of( coordinator, screen_rows );

		return frame.rows( ) > 0 &&
			buffer_row_text( frame, frame.rows( ) - 1 ).find( "── history" ) != std::string::npos;
	}

}

TEST_CASE( "a page of history fills the viewport with retained rows",
	"[tui][render]" ) {
	auto coordinator = make_coordinator( );

	const auto retained = std::size_t{ 2 * HISTORY_WINDOW + 4 };
	commit_rows( coordinator, 0, retained );
	(void)coordinator.flush( );

	CHECK_FALSE( coordinator.scrolled( ) );

	REQUIRE( coordinator.state( ).scrollback.size( ) == retained );
	CHECK( coordinator.state( ).scrollback.front( ).front( ).text == "row 0" );
	CHECK( coordinator.state( ).scrollback.back( ).front( ).text ==
		"row " + std::to_string( retained - 1 ) );

	// PageUp asks for a screenful; every row above the indicator is a real retained row.
	coordinator.scroll_by( -static_cast< int >( HISTORY_WINDOW ) );
	REQUIRE( coordinator.scrolled( ) );

	CHECK( region_rows_for( coordinator.state( ), SCREEN_ROWS ) == HISTORY_REGION );
	CHECK( indicator_visible( coordinator, SCREEN_ROWS ) );

	const auto visible = viewport_rows( coordinator, SCREEN_ROWS );

	REQUIRE( visible.size( ) == HISTORY_WINDOW );
	CHECK( visible == expected_window( retained - 2 * HISTORY_WINDOW,
		retained - HISTORY_WINDOW - 1 ) );

	for ( const auto& row : visible ) {
		CHECK_FALSE( row.empty( ) );
	}

	const auto scrolled = coordinator.flush( );

	CHECK( scrolled.find( "> " ) == std::string::npos );

	coordinator.scroll_to_bottom( );
	CHECK_FALSE( coordinator.scrolled( ) );

	const auto live = coordinator.flush( );

	CHECK( live.find( "── history" ) == std::string::npos );
	CHECK( live.find( "> " ) != std::string::npos );
}

TEST_CASE( "the history window ends just above the live end at any offset",
	"[tui][render]" ) {
	auto coordinator = make_coordinator( );

	const auto retained = std::size_t{ 50 };
	commit_rows( coordinator, 0, retained );
	(void)coordinator.flush( );

	// The window ends `offset` rows above the newest retained row, so a deeper offset
	// never shows a row the buffer does not hold.
	for ( const auto offset : std::array< std::size_t, 3 >{ 1, 10, 21 } ) {
		coordinator.scroll_to_bottom( );
		coordinator.scroll_by( -static_cast< int >( offset ) );

		REQUIRE( coordinator.scrolled( ) );

		const auto newest = retained - offset;

		CHECK( viewport_rows( coordinator, SCREEN_ROWS ) ==
			expected_window( newest - HISTORY_WINDOW, newest - 1 ) );
	}
}

TEST_CASE( "a scroll past the top stops with the oldest row on the top row",
	"[tui][render]" ) {
	auto coordinator = make_coordinator( );

	commit_rows( coordinator, 0, 50 );
	(void)coordinator.flush( );

	// A scroll far past the top clamps to the retained range, and the window stays full.
	coordinator.scroll_by( -1000000 );
	REQUIRE( coordinator.scrolled( ) );

	const auto visible = viewport_rows( coordinator, SCREEN_ROWS );

	REQUIRE( visible.size( ) == HISTORY_WINDOW );
	CHECK( visible.front( ) == "row 0" );
	CHECK( visible == expected_window( 0, HISTORY_WINDOW - 1 ) );

	coordinator.scroll_by( -1000000 );
	CHECK( viewport_rows( coordinator, SCREEN_ROWS ) == visible );
}

TEST_CASE( "a buffer shorter than the viewport still pages into history",
	"[tui][render]" ) {
	auto coordinator = make_coordinator( );

	// The whole buffer fits the viewport, so PageUp opens the history view one row deep.
	const auto retained = std::size_t{ 5 };
	commit_rows( coordinator, 0, retained );
	(void)coordinator.flush( );

	coordinator.scroll_by( -1000000 );
	REQUIRE( coordinator.scrolled( ) );

	const auto visible = viewport_rows( coordinator, SCREEN_ROWS );

	// Every retained row but the newest is shown, oldest hugging the indicator.
	const auto shown = retained - 1;
	auto expected = std::vector< std::string >( HISTORY_WINDOW - shown, std::string{ } );

	for ( auto index = std::size_t{ 0 }; index < shown; ++index ) {
		expected.push_back( "row " + std::to_string( index ) );
	}

	REQUIRE( visible.size( ) == HISTORY_WINDOW );
	CHECK( visible == expected );

	auto single = make_coordinator( );

	commit_rows( single, 0, 1 );
	(void)single.flush( );

	single.scroll_by( -1000000 );
	CHECK_FALSE( single.scrolled( ) );

	const auto frame = frame_of( single, SCREEN_ROWS );

	CHECK( buffer_contains( frame, "> " ) );
	CHECK_FALSE( buffer_contains( frame, "── history" ) );
}

TEST_CASE( "scrolling toward the live view clamps at the bottom", "[tui][render]" ) {
	auto coordinator = make_coordinator( );

	commit_rows( coordinator, 0, 50 );
	(void)coordinator.flush( );

	// already at the bottom: scrolling further down is a no-op, not a negative.
	coordinator.scroll_by( 100 );
	CHECK_FALSE( coordinator.scrolled( ) );

	coordinator.scroll_by( -1 );
	REQUIRE( coordinator.scrolled( ) );

	coordinator.scroll_by( 100 );
	CHECK_FALSE( coordinator.scrolled( ) );
}

TEST_CASE( "committing while scrolled leaves the viewport where it is",
	"[tui][render]" ) {
	auto coordinator = make_coordinator( );

	commit_rows( coordinator, 0, 50 );
	(void)coordinator.flush( );

	coordinator.scroll_by( -10 );
	(void)coordinator.flush( );

	const auto before = viewport_rows( coordinator, SCREEN_ROWS );

	// a commit lands while scrolled: it appends, it does not move the view.
	commit_rows( coordinator, 50, 5 );
	(void)coordinator.flush( );

	REQUIRE( coordinator.scrolled( ) );

	const auto after = viewport_rows( coordinator, SCREEN_ROWS );

	CHECK( after == before );
	CHECK( after != expected_window( 50, 50 + HISTORY_WINDOW - 1 ) );
}

TEST_CASE( "a commit while scrolled does not move a short-buffer viewport",
	"[tui][render]" ) {
	auto coordinator = make_coordinator( );

	// The buffer is shorter than the viewport, so the viewport is pinned to the oldest
	// rows and the offset advances with the append to keep them in place.
	commit_rows( coordinator, 0, 5 );
	(void)coordinator.flush( );

	coordinator.scroll_by( -1000000 );
	REQUIRE( coordinator.scrolled( ) );

	const auto before = viewport_rows( coordinator, SCREEN_ROWS );

	commit_rows( coordinator, 5, 3 );
	(void)coordinator.flush( );

	REQUIRE( coordinator.scrolled( ) );

	const auto after = viewport_rows( coordinator, SCREEN_ROWS );

	for ( auto index = std::size_t{ 0 }; index < before.size( ); ++index ) {
		if ( !before[ index ].empty( ) ) {
			CHECK( after[ index ] == before[ index ] );
		}
	}
}

TEST_CASE( "the retained ring keeps the newest rows when it overflows",
	"[tui][render]" ) {
	auto coordinator = make_coordinator( );

	const auto overflow = std::size_t{ 10 };
	const auto total = SCROLLBACK_MAX_ROWS + overflow;

	commit_rows( coordinator, 0, total );
	(void)coordinator.flush( );

	REQUIRE( coordinator.state( ).scrollback.size( ) == SCROLLBACK_MAX_ROWS );
	CHECK( coordinator.state( ).scrollback.back( ).front( ).text ==
		"row " + std::to_string( total - 1 ) );

	coordinator.scroll_by( -1000000 );
	REQUIRE( coordinator.scrolled( ) );

	const auto visible = viewport_rows( coordinator, SCREEN_ROWS );

	REQUIRE( visible.size( ) == HISTORY_WINDOW );

	for ( const auto& row : visible ) {
		CHECK_FALSE( row.empty( ) );
	}
}

TEST_CASE( "a scrolled viewport survives a resize with its window full",
	"[tui][render]" ) {
	auto coordinator = make_coordinator( );

	commit_rows( coordinator, 0, 50 );
	(void)coordinator.flush( );

	coordinator.scroll_by( -1000000 );
	REQUIRE( coordinator.scrolled( ) );

	// The viewport's height is the screen's, so the window stays full rather than
	// leaving blank rows above the history.
	coordinator.resize( SCREEN_ROWS + 10, SCREEN_COLUMNS );
	(void)coordinator.flush( );

	const auto taller = viewport_rows( coordinator, SCREEN_ROWS + 10 );

	REQUIRE( taller.size( ) == SCREEN_ROWS + 10 - 2 );
	CHECK( taller.front( ) == "row 0" );

	coordinator.resize( SCREEN_ROWS - 10, SCREEN_COLUMNS );
	(void)coordinator.flush( );

	const auto shorter = viewport_rows( coordinator, SCREEN_ROWS - 10 );

	REQUIRE( shorter.size( ) == SCREEN_ROWS - 10 - 2 );

	for ( const auto& row : shorter ) {
		CHECK_FALSE( row.empty( ) );
	}
}

TEST_CASE( "a shrinking region erases every old row before repainting",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	auto text = std::string{ };

	for ( auto index = std::size_t{ 0 }; index <= 40; ++index ) {
		text += "line " + std::to_string( index ) + "\n";
	}

	text += "line 41";

	auto delta = event_queue::item{ };
	delta.type = event_queue::kind::assistant_delta;
	delta.text = text;
	coordinator.apply( delta );

	(void)coordinator.flush( );

	REQUIRE( region_rows_for( coordinator.state( ), 30 ) == LIVE_REGION_MAX_ROWS );

	coordinator.resize( 12, 60 );

	REQUIRE( region_rows_for( coordinator.state( ), 12 ) < LIVE_REGION_MAX_ROWS );

	const auto bytes = coordinator.flush( );

	// every row of the region is erased, so nothing survives below the new one.
	CHECK( count_occurrences( bytes, "\x1b[2K" ) == LIVE_REGION_MAX_ROWS );
	CHECK( bytes.find( "\x1b[15A" ) != std::string::npos );

	const auto short_region = region_rows_for( coordinator.state( ), 12 );
	const auto frame = build_frame( coordinator.state( ), short_region, 59, 1 );

	CHECK( buffer_contains( frame, "line 41" ) );
	CHECK_FALSE( buffer_contains( frame, "line 0" ) );
}

TEST_CASE( "a resize erases the region even when its height is unchanged",
	"[tui][render]" ) {
	auto coordinator = render_coordinator{ };

	auto caps = capabilities{ };
	caps.depth = capabilities::color_depth::none;
	coordinator.set_capabilities( caps );
	coordinator.resize( 30, 60 );

	coordinator.set_prompt( "hi", 2 );
	(void)coordinator.flush( );

	REQUIRE( region_rows_for( coordinator.state( ), 30 ) == 2 );
	REQUIRE( region_rows_for( coordinator.state( ), 10 ) == 2 );

	coordinator.resize( 10, 60 );

	const auto bytes = coordinator.flush( );

	CHECK( bytes.find( "\x1b[2K" ) != std::string::npos );
}

TEST_CASE( "no flush touches the terminal's own scrollback", "[tui][render]" ) {
	auto coordinator = make_coordinator( );

	const auto forbidden = [ ]( const std::string& bytes ) {
		CHECK( bytes.find( "\x1b[2J" ) == std::string::npos );
		CHECK( bytes.find( "\x1b[3J" ) == std::string::npos );
		CHECK( bytes.find( "\x1b[S" ) == std::string::npos );
		CHECK( bytes.find( "\x1b[T" ) == std::string::npos );
	};

	commit_rows( coordinator, 0, 50 );
	forbidden( coordinator.flush( ) );

	coordinator.scroll_by( -20 );
	REQUIRE( coordinator.scrolled( ) );
	forbidden( coordinator.flush( ) );

	commit_rows( coordinator, 50, 5 );
	forbidden( coordinator.flush( ) );

	coordinator.resize( 12, 60 );
	forbidden( coordinator.flush( ) );

	coordinator.scroll_to_bottom( );
	forbidden( coordinator.flush( ) );
}
