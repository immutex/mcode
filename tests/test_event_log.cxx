#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "mcode/agent/loop.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	auto scratch_dir( ) -> std::filesystem::path {
		return test::scratch_directory( "mcode-eventlog-test" );
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
	const auto directory = scratch_dir( );
	const auto path = directory / "session.jsonl";

	auto log = event_log{ };
	REQUIRE( static_cast< bool >( log.open( path ) ) );

	log.append( "session.start", R"({"goal":"test"})" );
	log.append( "tool.call", R"({"name":"read"})" );
	log.append( "tool.result", R"({"ok":true})" );

	const auto on_disk = read_all( path );
	REQUIRE( line_count( on_disk ) == 3 );
	REQUIRE( on_disk.find( "session.start" ) != std::string::npos );
	REQUIRE( on_disk.find( "tool.result" ) != std::string::npos );

	auto replayed = replay_event_log( path );
	REQUIRE( static_cast< bool >( replayed ) );
	REQUIRE( replayed->events_read == 3 );
	REQUIRE_FALSE( replayed->truncated_tail );
	REQUIRE( replayed->malformed_lines == 0 );

	log.close( );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "a torn final line is reported, not fatal", "[eventlog]" ) {
	const auto directory = scratch_dir( );
	const auto path = directory / "torn.jsonl";

	auto log = event_log{ };
	REQUIRE( static_cast< bool >( log.open( path ) ) );

	log.append( "session.start" );
	log.append( "tool.call" );
	log.close( );

	{
		auto out = std::ofstream{ path, std::ios::binary | std::ios::app };
		out << R"({"v":1,"seq":2,"kind":"tool.resul)";
	}

	auto replayed = replay_event_log( path );
	REQUIRE( static_cast< bool >( replayed ) );

	REQUIRE( replayed->events_read == 2 );
	REQUIRE( replayed->truncated_tail );
	REQUIRE( replayed->malformed_lines == 0 );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "replay restores event payloads", "[eventlog]" ) {
	const auto directory = scratch_dir( );
	const auto path = directory / "payloads.jsonl";

	{
		auto out = std::ofstream{ path, std::ios::binary };
		out << R"({"v":1,"seq":0,"kind":"session.start","payload":{"headless":true}})" << "\n";
		out << R"({"v":1,"seq":1,"kind":"tool.call","payload":{"tool":"read"}})" << "\n";
	}

	auto replayed = replay_event_log( path );
	REQUIRE( static_cast< bool >( replayed ) );
	REQUIRE( replayed->events_read == 2 );
	REQUIRE( replayed->log.events( ).front( ).payload_json == R"({"headless":true})" );
	REQUIRE( replayed->log.events( ).back( ).payload_json == R"({"tool":"read"})" );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "a corrupt line is counted separately from a torn tail", "[eventlog]" ) {
	// a torn tail is an interrupted write; an interior malformed line means the file is corrupt
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
	// a resumed session must adopt the existing numbering or the log stops being a total order
	const auto directory = scratch_dir( );
	const auto path = directory / "resumed.jsonl";

	{
		auto first = event_log{ };
		REQUIRE( static_cast< bool >( first.open( path ) ) );
		first.append( "session.start" );
		first.append( "turn.start" );
		REQUIRE( first.next_sequence( ) == 2 );
	}

	auto second = event_log{ };
	REQUIRE( static_cast< bool >( second.open( path ) ) );
	REQUIRE( second.next_sequence( ) == 2 );

	const auto appended = second.append( "turn.end" );
	REQUIRE( appended.sequence == 2 );
	second.close( );

	const auto text = read_all( path );
	REQUIRE( line_count( text ) == 3 );
	REQUIRE( text.find( "\"seq\":2" ) != std::string::npos );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "opening a second log replaces the first, it does not merge",
	"[eventlog]" ) {
	// Switching sessions reuses one `event_log` object and repoints it at another
	// file, because the loop holds a pointer to it. `open` cleared its state only
	// when the target file was empty, so opening a non-empty second file kept the
	// first file's events in memory: `restore_transcript` then rebuilt a
	// conversation from two different sessions concatenated.
	const auto directory = scratch_dir( );

	const auto first_path = directory / "first.jsonl";
	const auto second_path = directory / "second.jsonl";

	{
		auto writer = event_log{ };
		REQUIRE( static_cast< bool >( writer.open( first_path ) ) );
		writer.append( "first.event" );
		writer.close( );

		REQUIRE( static_cast< bool >( writer.open( second_path ) ) );
		writer.append( "second.event" );
		writer.close( );
	}

	auto reopened = event_log{ };
	REQUIRE( static_cast< bool >( reopened.open( second_path ) ) );

	REQUIRE( reopened.size( ) == 1 );
	REQUIRE( reopened.events( ).front( ).kind == "second.event" );

	// The sequence restarts with the new file, because the two logs are
	// independent sessions rather than one continuing record.
	REQUIRE( reopened.next_sequence( ) == 1 );

	// Closed before the directory goes: Windows will not remove a file a handle
	// is still open on, and the failure reads as a broken test rather than a
	// held handle.
	reopened.close( );

	std::filesystem::remove_all( directory );
}

TEST_CASE( "an unopened log still works in memory", "[eventlog]" ) {
	// the log is usable without a file so a session that cannot write degrades
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

TEST_CASE( "a long session does not retain every payload in memory", "[eventlog]" ) {
	// `events_` held every payload for the life of the process -- a session that
	// reads a few hundred files kept all of them. The file is the record and the
	// vector is a cache, so the cache is now bounded and the oldest go first.
	const auto path = scratch_dir( ) / "bounded.jsonl";
	std::filesystem::remove( path );

	auto log = event_log{ };

	REQUIRE( static_cast< bool >( log.open( path ) ) );

	// One event larger than the whole cap, then many small ones.
	const auto big = std::string( MAX_RETAINED_LOG_BYTES + 1024, 'x' );
	log.append( "tool.output", big );

	for ( auto index = 0; index < 200; ++index ) {
		log.append( "tool.call", "{\"index\":" + std::to_string( index ) + "}" );
	}

	CHECK( log.evicted( ) > 0 );

	// Bounded, not emptied: the newest are still there for a reader.
	CHECK( log.events( ).size( ) < 201 );

	// Every event is on disk regardless of what memory kept -- that is the point.
	const auto on_disk = line_count( read_all( path ) );

	CHECK( on_disk == 201 );

	// A reopen replays the file whole, so nothing was lost by the eviction.
	auto reopened = event_log{ };

	REQUIRE( static_cast< bool >( reopened.open( path ) ) );
	CHECK( reopened.events( ).size( ) == 201 );
}
