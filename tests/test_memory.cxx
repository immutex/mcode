#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "mcode/fs/workspace.hxx"
#include "mcode/memory/ledger.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	auto scratch_workspace( const std::string_view name ) -> std::filesystem::path {
		const auto directory = test::scratch_directory( "mcode-ledger-test" ) / name;
		std::filesystem::remove_all( directory );
		std::filesystem::create_directories( directory );

		return directory;
	}

}

TEST_CASE( "the ledger lives outside the workspace", "[memory]" ) {
	const auto root = scratch_workspace( "outside" );
	const auto path = memory::ledger_path( root );

	REQUIRE( path.has_value( ) );

	// A ledger a repository could ship would be a file arriving from someone else
	// that the prompt then reads as trusted, which is the whole problem the trust
	// gate exists to solve.
	REQUIRE( !path_is_within( root, *path ) );
}

TEST_CASE( "two workspaces do not share a ledger", "[memory]" ) {
	const auto first = memory::ledger_path( scratch_workspace( "one" ) );
	const auto second = memory::ledger_path( scratch_workspace( "two" ) );

	REQUIRE( first.has_value( ) );
	REQUIRE( second.has_value( ) );
	REQUIRE( *first != *second );
}

TEST_CASE( "an absent ledger reads as empty, not as an error", "[memory]" ) {
	const auto text = memory::read_ledger( scratch_workspace( "absent" ) );

	REQUIRE( text.has_value( ) );
	REQUIRE( text->empty( ) );
}

TEST_CASE( "a note is appended verbatim under a dated heading", "[memory]" ) {
	const auto root = scratch_workspace( "append" );
	const auto text = "Release builds need `cmake --preset release`.";

	REQUIRE( memory::append_note( root, text, "user" ).has_value( ) );

	const auto ledger = memory::read_ledger( root );

	REQUIRE( ledger.has_value( ) );

	// Verbatim: an extraction pass is where the measured form degradation comes
	// from, so nothing rewrites the user's words on the way in.
	REQUIRE( ledger->find( text ) != std::string::npos );
	REQUIRE( ledger->find( "## " ) == 0 );
	REQUIRE( ledger->find( "— user" ) != std::string::npos );
}

TEST_CASE( "notes accumulate in order", "[memory]" ) {
	const auto root = scratch_workspace( "accumulate" );

	REQUIRE( memory::append_note( root, "first", "user" ).has_value( ) );
	REQUIRE( memory::append_note( root, "second", "correction" ).has_value( ) );

	const auto ledger = memory::read_ledger( root );

	REQUIRE( ledger.has_value( ) );
	REQUIRE( ledger->find( "first" ) < ledger->find( "second" ) );
}

TEST_CASE( "an empty note is refused", "[memory]" ) {
	REQUIRE( !memory::append_note( scratch_workspace( "empty" ), "", "user" ).has_value( ) );
}

TEST_CASE( "the ledger has a hard line cap that names the file", "[memory]" ) {
	const auto root = scratch_workspace( "cap" );

	// A line cap counts lines, so the note has to have them: a single long line is
	// one line and trips nothing.
	auto huge = std::string{ };

	for ( auto index = std::size_t{ 0 }; index < memory::MAX_NOTES_LINES + 10; ++index ) {
		huge += "note line\n";
	}

	const auto written = memory::append_note( root, huge, "user" );

	REQUIRE( !written.has_value( ) );
	REQUIRE( written.error( ).msg.find( "prune" ) != std::string::npos );
}

TEST_CASE( "a note under the cap is accepted", "[memory]" ) {
	const auto root = scratch_workspace( "under-cap" );

	REQUIRE( memory::append_note( root, "a short note", "user" ).has_value( ) );
	REQUIRE( memory::append_note( root, "another", "user" ).has_value( ) );

	const auto ledger = memory::read_ledger( root );

	REQUIRE( ledger.has_value( ) );
	REQUIRE( memory::line_count( *ledger ) < memory::MAX_NOTES_LINES );
}
