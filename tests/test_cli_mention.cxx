#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/cli/slash.hxx"
#include "mcode/tui/mention.hxx"
#include "mcode/tui/palette.hxx"

using namespace mcode;

namespace {

	auto files( ) -> const std::vector< std::string >& {
		static const auto LIST = std::vector< std::string >{
			"src/main.cxx",
			"src/model/client.cxx",
			"docs/notes.md",
		};

		return LIST;
	}

}

TEST_CASE( "a mention starts a word and closes on a space", "[cli][mention]" ) {
	const auto query = tui::mention_query( "see @src/f" );

	REQUIRE( query.has_value( ) );
	CHECK( *query == "src/f" );

	CHECK_FALSE( tui::mention_query( "a@b" ).has_value( ) );
	CHECK_FALSE( tui::mention_query( "@x y" ).has_value( ) );
	CHECK_FALSE( tui::mention_query( "no mention here" ).has_value( ) );
	CHECK_FALSE( tui::mention_query( "" ).has_value( ) );
}

TEST_CASE( "completing a mention inserts the path, never the contents",
	"[cli][mention]" ) {
	CHECK( tui::mention_completion( "see @src/f", "src/foo.cxx" ) == "see @src/foo.cxx " );
	CHECK( tui::mention_completion( "plain text", "src/foo.cxx" ) == "plain text" );
}

TEST_CASE( "a prefix match outranks a subsequence match", "[cli][mention]" ) {
	const auto ranked = tui::rank_mentions( files( ), "src/m", 8 );

	REQUIRE( ranked.size( ) == 2 );
	CHECK( ranked[ 0 ] == "src/main.cxx" );
	CHECK( ranked[ 1 ] == "src/model/client.cxx" );
}

TEST_CASE( "a basename prefix outranks a mid-path subsequence", "[cli][mention]" ) {
	const auto ranked = tui::rank_mentions( files( ), "client", 8 );

	REQUIRE( ranked.size( ) == 1 );
	CHECK( ranked[ 0 ] == "src/model/client.cxx" );
}

TEST_CASE( "an empty query keeps the input order and honours the cap",
	"[cli][mention]" ) {
	const auto ranked = tui::rank_mentions( files( ), "", 2 );

	REQUIRE( ranked.size( ) == 2 );
	CHECK( ranked[ 0 ] == "src/main.cxx" );
	CHECK( ranked[ 1 ] == "src/model/client.cxx" );
}

TEST_CASE( "a query matching no path yields nothing", "[cli][mention]" ) {
	CHECK( tui::rank_mentions( files( ), "zzzzz", 8 ).empty( ) );
}

TEST_CASE( "the mention picker fills the same palette with a cleared prefix",
	"[cli][mention]" ) {
	auto palette = tui::slash_palette{ };
	auto index = tui::mention_index{ nullptr };

	cli::refresh_mention_palette( palette, index, "@" );

	// No workspace is a picker with no files, not an error.
	CHECK( palette.open );
	CHECK( palette.empty( ) );
	CHECK( palette.prefix.empty( ) );

	cli::refresh_mention_palette( palette, index, "hello" );

	CHECK_FALSE( palette.open );
}

