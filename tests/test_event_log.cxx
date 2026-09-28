#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "mcode/agent/loop.hxx"

using namespace mcode;

namespace {

	auto scratch_dir( ) -> std::filesystem::path {
		auto path = std::filesystem::temp_directory_path( ) / "mcode-eventlog-test";
		std::filesystem::remove_all( path );
		std::filesystem::create_directories( path );

		return path;
	}

	auto read_all( const std::filesystem::path& path ) -> std::string {
		auto stream = std::ifstream{ path, std::ios::binary };
		auto buffer = std::ostringstream{ };
		buffer << stream.rdbuf( );

		return buffer.str( );
	}

	auto line_count( const std::string_view text ) -> std::size_t {
		auto count = std::size_t{ 0 };

		for ( const auto character : text ) {
			if ( character == '\n' ) {
				++count;
			}
		}

		return count;
	}

}

TEST_CASE( "every event is flushed as it is appended", "[eventlog]" ) {
	// E2's acceptance: a crash mid-run replays cleanly. The mechanism is that
	// nothing is buffered, so the file on disk is always complete up to the last
	// append. This reads the file WITHOUT closing the log, which is the state a
	// crash leaves behind.
	const auto directory = scratch_dir( );
	const auto path = directory / "session.jsonl";

	auto log = event_log{ };
	REQUIRE( static_cast< bool >( log.open( path ) ) );

	log.append( "session.start", R"({"goal":"test"})" );
	log.append( "tool.call", R"({"name":"read"})" );
	log.append( "tool.result", R"({"ok":true})" );

	// No close, no flush: read what a crash would leave.
	const auto on_disk = read_all( path );
	REQUIRE( line_count( on_disk ) == 3 );
	REQUIRE( on_disk.find( "session.start" ) != std::string::npos );
	REQUIRE( on_disk.find( "tool.result" ) != std::string::npos );

	auto replayed = replay_event_log( path );
	REQUIRE( static_cast< bool >( replayed ) );
	REQUIRE( replayed->events_read == 3 );
	REQUIRE_FALSE( replayed->truncated_tail );
	REQUIRE( replayed->malformed_lines == 0 );

	// The log is deliberately still open here -- that is the crash state being
	// asserted -- so it must be closed before the directory can be removed on
	// Windows.
	log.close( );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "a torn final line is reported, not fatal", "[eventlog]" ) {
	// The realistic crash: the process dies mid-write. Discarding the whole
	// session because of one truncated line would lose exactly the evidence a
	// post-mortem wants.
	const auto directory = scratch_dir( );
	const auto path = directory / "torn.jsonl";

	auto log = event_log{ };
	REQUIRE( static_cast< bool >( log.open( path ) ) );

	log.append( "session.start" );
	log.append( "tool.call" );
	log.close( );

	// Simulate the torn write.
	{
		auto out = std::ofstream{ path, std::ios::binary | std::ios::app };
		out << R"({"v":1,"seq":2,"kind":"tool.resul)";
	}

	auto replayed = replay_event_log( path );
	REQUIRE( static_cast< bool >( replayed ) );

	// The two complete events survive.
	REQUIRE( replayed->events_read == 2 );
	REQUIRE( replayed->truncated_tail );
	REQUIRE( replayed->malformed_lines == 0 );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "a corrupt line is counted separately from a torn tail", "[eventlog]" ) {
	// These are different problems: a torn tail is an interrupted write, a
	// malformed interior line means the file is not what it claims to be.
	const auto directory = scratch_dir( );
	const auto path = directory / "corrupt.jsonl";

	{
		auto out = std::ofstream{ path, std::ios::binary };
		out << R"({"v":1,"seq":0,"kind":"session.start","payload":{}})" << "\n";
		out << "this is not json\n";
		out << R"({"v":1,"seq":2,"kind":"tool.call","payload":{}})" << "\n";
	}

	auto replayed = replay_event_log( path );
	REQUIRE( static_cast< bool >( replayed ) );
	REQUIRE( replayed->events_read == 2 );
	REQUIRE( replayed->malformed_lines == 1 );
	REQUIRE_FALSE( replayed->truncated_tail );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "reopening a log continues its sequence numbering", "[eventlog]" ) {
	// A resumed session must not restart at zero and collide with what is already
	// on disk, or the log stops being a total order.
	const auto directory = scratch_dir( );
	const auto path = directory / "resumed.jsonl";

	{
		auto first = event_log{ };
		REQUIRE( static_cast< bool >( first.open( path ) ) );
		first.append( "session.start" );
		first.append( "turn.start" );
		REQUIRE( first.next_sequence( ) == 2 );
	}

	// A second log over the same file adopts the existing numbering.
	auto second = event_log{ };
	REQUIRE( static_cast< bool >( second.open( path ) ) );
	REQUIRE( second.next_sequence( ) == 2 );

	const auto appended = second.append( "turn.end" );
	REQUIRE( appended.sequence == 2 );
	second.close( );

	// Three lines, sequences 0, 1, 2 with no duplicates.
	const auto text = read_all( path );
	REQUIRE( line_count( text ) == 3 );
	REQUIRE( text.find( "\"seq\":2" ) != std::string::npos );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "an unopened log still works in memory", "[eventlog]" ) {
	// The log must be usable without a file -- the smoke test and unit tests rely
	// on it, and a session that cannot write should degrade rather than fail.
	auto log = event_log{ };

	REQUIRE_FALSE( log.is_open( ) );

	log.append( "session.start" );
	log.append( "tool.call" );

	REQUIRE( log.size( ) == 2 );
	REQUIRE( log.write_failures( ) == 0 );

	const auto jsonl = log.to_jsonl( );
	REQUIRE( line_count( jsonl ) == 2 );
}

TEST_CASE( "an unwritable path fails at open, not at append", "[eventlog]" ) {
	auto log = event_log{ };

	// A path whose parent is an existing FILE cannot be created.
	const auto directory = scratch_dir( );
	const auto blocker = directory / "not-a-directory";
	{
		auto out = std::ofstream{ blocker, std::ios::binary };
		out << "x";
	}

	auto opened = log.open( blocker / "session.jsonl" );
	REQUIRE_FALSE( static_cast< bool >( opened ) );
	REQUIRE( opened.error( ).code == errc::io );

	std::filesystem::remove_all( directory );
}
