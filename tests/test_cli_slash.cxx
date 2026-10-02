#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
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

TEST_CASE( "a leading slash with no space is parsed as a command", "[cli][slash]" ) {
	const auto match = cli::match_command( "/help", commands( ) );

	CHECK( match.is_command );
	REQUIRE( match.entry != nullptr );
	CHECK( match.name == "help" );
	CHECK( match.arguments.empty( ) );
}

TEST_CASE( "everything after the first space is the argument string",
	"[cli][slash]" ) {
	const auto match = cli::match_command( "/help me now", commands( ) );

	CHECK( match.is_command );
	REQUIRE( match.entry != nullptr );
	CHECK( match.name == "help" );
	CHECK( match.arguments == "me now" );
}

TEST_CASE( "a line without a leading slash is not a command", "[cli][slash]" ) {
	CHECK_FALSE( cli::match_command( "hello", commands( ) ).is_command );
}

TEST_CASE( "an unknown name is still a command but resolves to no entry",
	"[cli][slash]" ) {
	const auto match = cli::match_command( "/unknown", commands( ) );

	CHECK( match.is_command );
	CHECK( match.entry == nullptr );
	CHECK( match.name == "unknown" );
}

TEST_CASE( "every built-in command resolves by its own name", "[cli][slash]" ) {
	const auto& builtins = cli::builtin_commands( );

	REQUIRE_FALSE( builtins.empty( ) );

	for ( const auto& entry : builtins ) {
		const auto match = cli::match_command( "/" + entry.name, builtins );

		CHECK( match.is_command );
		REQUIRE( match.entry != nullptr );
		CHECK( match.entry->name == entry.name );
	}
}
