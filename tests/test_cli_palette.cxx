#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/cli/slash.hxx"
#include "mcode/tui/palette.hxx"

using namespace mcode;

namespace {

	auto commands( ) -> const std::vector< tui::slash_command >& {
		static const auto LIST = std::vector< tui::slash_command >{
			{ "help", "List the available commands" },
			{ "model", "Show the model" },
			{ "tools", "List the tools" },
			{ "remodel", "Re-read the model" },
		};

		return LIST;
	}

	auto submitted( ) -> const std::vector< std::string >& {
		static const auto LIST = std::vector< std::string >{
			"run the tests",
			"fix the parser",
			"run the linter",
		};

		return LIST;
	}

}

TEST_CASE( "an empty query lists every command in registration order",
	"[cli][palette]" ) {
	const auto matches = tui::filter_commands( commands( ), "" );

	REQUIRE( matches.size( ) == 4 );
	CHECK( matches[ 0 ].name == "help" );
	CHECK( matches[ 1 ].name == "model" );
	CHECK( matches[ 2 ].name == "tools" );
	CHECK( matches[ 3 ].name == "remodel" );
}

TEST_CASE( "a prefix match outranks a command that merely contains the query",
	"[cli][palette]" ) {
	const auto matches = tui::filter_commands( commands( ), "mo" );

	REQUIRE( matches.size( ) == 2 );
	CHECK( matches[ 0 ].name == "model" );
	CHECK( matches[ 1 ].name == "remodel" );
}

TEST_CASE( "a query matching nothing yields an empty list", "[cli][palette]" ) {
	CHECK( tui::filter_commands( commands( ), "zzz" ).empty( ) );
}

TEST_CASE( "a bare slash is a command query, a path is not", "[cli][palette]" ) {
	const auto bare = tui::command_query( "/" );

	REQUIRE( bare.has_value( ) );
	CHECK( bare->empty( ) );

	const auto partial = tui::command_query( "/he" );

	REQUIRE( partial.has_value( ) );
	CHECK( *partial == "he" );
}

TEST_CASE( "a space, a second slash, or no slash ends the command query",
	"[cli][palette]" ) {
	CHECK_FALSE( tui::command_query( "/help " ).has_value( ) );
	CHECK_FALSE( tui::command_query( "/tmp/notes" ).has_value( ) );
	CHECK_FALSE( tui::command_query( "notacommand" ).has_value( ) );
	CHECK_FALSE( tui::command_query( "" ).has_value( ) );
}

TEST_CASE( "completing a name leaves room for arguments", "[cli][palette]" ) {
	CHECK( tui::completed_command( "help" ) == "/help " );
}

TEST_CASE( "the palette opens on a command query and closes when it is not one",
	"[cli][palette]" ) {
	auto palette = tui::slash_palette{ };

	cli::refresh_palette( palette, commands( ), "/" );

	CHECK( palette.open );
	CHECK( palette.matches.size( ) == 4 );

	cli::refresh_palette( palette, commands( ), "/mo" );

	CHECK( palette.open );
	REQUIRE( palette.matches.size( ) == 2 );
	CHECK( palette.matches.front( ).name == "model" );

	cli::refresh_palette( palette, commands( ), "hello" );

	CHECK_FALSE( palette.open );
	CHECK( palette.matches.empty( ) );
}

TEST_CASE( "a selection past the end of a narrowed list resets to the top",
	"[cli][palette]" ) {
	auto palette = tui::slash_palette{ };

	cli::refresh_palette( palette, commands( ), "/" );
	palette.selected = 2;

	cli::refresh_palette( palette, commands( ), "/mo" );

	REQUIRE( palette.matches.size( ) == 2 );
	CHECK( palette.selected == 0 );
}

TEST_CASE( "Enter on a highlighted row runs that row's command, not the typed text",
	"[cli][palette]" ) {
	auto palette = tui::slash_palette{ };

	cli::refresh_palette( palette, commands( ), "/" );

	REQUIRE( palette.matches.size( ) == 4 );
	CHECK( tui::submitted_line( palette, "/" ) == "/help" );

	palette.selected = 1;
	CHECK( tui::submitted_line( palette, "/" ) == "/model" );

	// with no highlighted row the typed text is echoed through.
	palette.matches.clear( );
	CHECK( tui::submitted_line( palette, "/zzz" ) == "/zzz" );

	auto closed = tui::slash_palette{ };
	CHECK( tui::submitted_line( closed, "/model extra" ) == "/model extra" );
}

TEST_CASE( "Tab inserts the completed command and leaves room for arguments",
	"[cli][palette]" ) {
	auto palette = tui::slash_palette{ };

	cli::refresh_palette( palette, commands( ), "/mo" );

	const auto* row = palette.highlighted( );

	REQUIRE( row != nullptr );
	CHECK( tui::completed_command( row->name ) == "/model " );
}

TEST_CASE( "an empty history query is the most recent entries, newest first",
	"[cli][history]" ) {
	const auto ranked = cli::rank_history( submitted( ), "", 8 );

	REQUIRE( ranked.size( ) == 3 );
	CHECK( ranked[ 0 ] == "run the linter" );
	CHECK( ranked[ 1 ] == "fix the parser" );
	CHECK( ranked[ 2 ] == "run the tests" );
}

TEST_CASE( "a history prefix match outranks a later substring match",
	"[cli][history]" ) {
	const auto ranked = cli::rank_history( submitted( ), "run", 8 );

	REQUIRE( ranked.size( ) == 2 );
	CHECK( ranked[ 0 ] == "run the linter" );
	CHECK( ranked[ 1 ] == "run the tests" );
}

TEST_CASE( "a history query that is only a substring still matches",
	"[cli][history]" ) {
	const auto ranked = cli::rank_history( submitted( ), "parser", 8 );

	REQUIRE( ranked.size( ) == 1 );
	CHECK( ranked[ 0 ] == "fix the parser" );
}

TEST_CASE( "a history query matching nothing yields nothing", "[cli][history]" ) {
	CHECK( cli::rank_history( submitted( ), "zzzzz", 8 ).empty( ) );
}

TEST_CASE( "a history query that is only a subsequence still matches, last",
	"[cli][history]" ) {
	const auto ranked = cli::rank_history( submitted( ), "rtl", 8 );

	// "run the linter" is a subsequence; the others are not.
	REQUIRE( ranked.size( ) == 1 );
	CHECK( ranked[ 0 ] == "run the linter" );
}

TEST_CASE( "the history palette opens on Ctrl+R and inserts rather than submits",
	"[cli][history]" ) {
	auto palette = tui::slash_palette{ };
	auto controller = cli::palette_controller{ { &commands( ), nullptr, &submitted( ) } };

	CHECK_FALSE( cli::submits( cli::palette_source::history ) );
	CHECK_FALSE( cli::submits( cli::palette_source::mentions ) );
	CHECK( cli::submits( cli::palette_source::commands ) );

	controller.open_history( palette, "" );

	CHECK( palette.open );
	CHECK( controller.source( ) == cli::palette_source::history );
	CHECK( palette.prefix.empty( ) );
	REQUIRE( palette.matches.size( ) == 3 );
	CHECK( palette.matches[ 0 ].name == "run the linter" );

	controller.refresh( palette, "parser" );

	REQUIRE( palette.matches.size( ) == 1 );
	CHECK( palette.matches[ 0 ].name == "fix the parser" );

	const auto* row = palette.highlighted( );

	REQUIRE( row != nullptr );
	CHECK( cli::inserted_line( controller.source( ), "parser", *row ) == "fix the parser" );

	controller.reset( );
	CHECK( controller.source( ) == cli::palette_source::commands );

	controller.refresh( palette, "parser" );
	CHECK_FALSE( palette.open );
}

TEST_CASE( "the command source is what a bare prompt implies",
	"[cli][history]" ) {
	auto palette = tui::slash_palette{ };
	auto controller = cli::palette_controller{ { &commands( ), nullptr, &submitted( ) } };

	controller.refresh( palette, "/" );

	CHECK( controller.source( ) == cli::palette_source::commands );
	CHECK( palette.prefix == "/" );
	CHECK( palette.matches.size( ) == 4 );

	controller.refresh( palette, "plain text" );

	CHECK_FALSE( palette.open );
	CHECK( controller.source( ) == cli::palette_source::commands );
}

TEST_CASE( "a list shorter than the window shows all of it and hides nothing",
	"[tui][palette]" ) {
	const auto window = tui::palette_window_for( 4, 2, 10 );

	CHECK( window.first == 0 );
	CHECK( window.count == 4 );
	CHECK_FALSE( window.hidden_above( ) );
	CHECK_FALSE( window.hidden_below( 4 ) );
}

TEST_CASE( "a list exactly the window's length shows all of it",
	"[tui][palette]" ) {
	const auto window = tui::palette_window_for( 10, 9, 10 );

	CHECK( window.first == 0 );
	CHECK( window.count == 10 );
	CHECK_FALSE( window.hidden_above( ) );
	CHECK_FALSE( window.hidden_below( 10 ) );
}

TEST_CASE( "the window pins to the top until the selection reaches its last row",
	"[tui][palette]" ) {
	for ( auto selected = std::size_t{ 0 }; selected < 10; ++selected ) {
		const auto window = tui::palette_window_for( 30, selected, 10 );

		CHECK( window.first == 0 );
		CHECK( window.count == 10 );
		CHECK( window.hidden_below( 30 ) );
	}

	// the row after the last visible one is the first that scrolls.
	const auto scrolled = tui::palette_window_for( 30, 10, 10 );

	CHECK( scrolled.first == 1 );
	CHECK( scrolled.count == 10 );
	CHECK( scrolled.hidden_above( ) );
	CHECK( scrolled.hidden_below( 30 ) );
}

TEST_CASE( "the window keeps the last row of the list at its last row",
	"[tui][palette]" ) {
	const auto window = tui::palette_window_for( 30, 29, 10 );

	CHECK( window.first == 20 );
	CHECK( window.count == 10 );
	CHECK( window.hidden_above( ) );
	CHECK_FALSE( window.hidden_below( 30 ) );
}

TEST_CASE( "an empty list yields an empty window rather than a negative span",
	"[tui][palette]" ) {
	const auto window = tui::palette_window_for( 0, 0, 10 );

	CHECK( window.first == 0 );
	CHECK( window.count == 0 );
	CHECK_FALSE( window.hidden_above( ) );
	CHECK_FALSE( window.hidden_below( 0 ) );
}

TEST_CASE( "a window of no rows shows nothing", "[tui][palette]" ) {
	const auto window = tui::palette_window_for( 30, 5, 0 );

	CHECK( window.first == 0 );
	CHECK( window.count == 0 );
}
