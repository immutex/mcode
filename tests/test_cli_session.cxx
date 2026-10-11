#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "mcode/cli/exec.hxx"

#include "test_scratch.hxx"

using namespace mcode;

TEST_CASE( "an unknown session id fails closed and names the id", "[cli][session]" ) {
	const auto workspace = test::scratch_directory( "mcode-session-unknown" );

	// A well-formed id for this workspace whose file was never written. The scratch
	// directory is unique, so nothing but this test can have created one.
	const auto absent = cli::session_id_for( workspace, 1 );

	auto unknown = cli::resolve_session( workspace, absent );
	REQUIRE_FALSE( static_cast< bool >( unknown ) );

	// `config` is the code the CLI maps to the usage exit, so a bad id is an argument
	// problem rather than a verification failure.
	CHECK( unknown.error( ).code == errc::config );
	CHECK( unknown.error( ).msg.find( absent ) != std::string::npos );

	// the run path -- what --resume actually reaches -- is the same check.
	auto options = cli::exec_options{ };
	options.resume_session = absent;

	auto refused = cli::open_session_for_run( options, workspace );
	REQUIRE_FALSE( static_cast< bool >( refused ) );
	CHECK( refused.error( ).code == errc::config );
	CHECK( refused.error( ).msg.find( absent ) != std::string::npos );

	// an id carrying a path separator is refused by name, never joined to a path.
	for ( const auto* id : { "../../etc/passwd", "..", "a/b", "C:\\sessions\\x" } ) {
		auto escaping = cli::resolve_session( workspace, id );
		REQUIRE_FALSE( static_cast< bool >( escaping ) );
		CHECK( escaping.error( ).code == errc::config );
		CHECK( escaping.error( ).msg.find( id ) != std::string::npos );
	}

	// a well-formed id belonging to another workspace is refused, not reopened.
	const auto elsewhere = test::scratch_directory( "mcode-session-elsewhere" );
	const auto foreign = cli::session_id_for( elsewhere, 1 );

	auto crossed = cli::resolve_session( workspace, foreign );
	REQUIRE_FALSE( static_cast< bool >( crossed ) );
	CHECK( crossed.error( ).code == errc::config );
	CHECK( crossed.error( ).msg.find( foreign ) != std::string::npos );

	// `--continue` with no session at all is an error too, never a silent fresh start.
	auto continuation = cli::exec_options{ };
	continuation.continue_session = true;

	auto missing = cli::open_session_for_run( continuation, workspace );
	REQUIRE_FALSE( static_cast< bool >( missing ) );
	CHECK( missing.error( ).code == errc::config );

	std::filesystem::remove_all( workspace );
	std::filesystem::remove_all( elsewhere );
}

TEST_CASE( "a session id is derived from the workspace and the start time", "[cli][session]" ) {
	const auto first = cli::session_id_for( "C:/workspace/one", 1'700'000'000'000 );
	const auto second = cli::session_id_for( "C:/workspace/two", 1'700'000'000'000 );
	const auto later = cli::session_id_for( "C:/workspace/one", 1'700'000'000'001 );

	// two workspaces cannot collide on one id, and the same workspace at a later time
	// gets a different one.
	CHECK( first != second );
	CHECK( first != later );

	// deterministic, so `--continue` reopens the same file a second run wrote.
	CHECK( cli::session_id_for( "C:/workspace/one", 1'700'000'000'000 ) == first );

	// filesystem-safe on every platform: hex digits, one dash, decimal digits.
	CHECK( first.find( '/' ) == std::string::npos );
	CHECK( first.find( '\\' ) == std::string::npos );
	CHECK( first.find( ':' ) == std::string::npos );
	CHECK( first.find( ' ' ) == std::string::npos );

	// the stamp is the suffix, so the names sort by time.
	CHECK( first.ends_with( "1700000000000" ) );
	CHECK( later.ends_with( "1700000000001" ) );
	CHECK( later.substr( 0, later.find( '-' ) ) == first.substr( 0, first.find( '-' ) ) );
}

TEST_CASE( "a run's session file is real JSONL under the state directory", "[cli][session]" ) {
	const auto workspace = test::scratch_directory( "mcode-session-write" );
	const auto state = cli::session_directory( );
	REQUIRE( static_cast< bool >( state ) );

	auto options = cli::exec_options{ };
	const auto session = cli::open_session_for_run( options, workspace );
	REQUIRE( static_cast< bool >( session ) );
	CHECK( session->path.parent_path( ) == *state );
	CHECK_FALSE( session->id.empty( ) );

	auto log = event_log{ };
	REQUIRE( static_cast< bool >( log.open( session->path ) ) );
	CHECK( log.path( ) == session->path );

	const auto report = cli::start_session_log( log, *session, false );
	CHECK( report.find( session->id ) != std::string::npos );

	log.append( "tool.call", R"({"name":"read"})" );
	log.close( );

	// The acceptance test for the format: parseable line by line, with nothing dropped.
	auto replayed = replay_event_log( session->path );
	REQUIRE( static_cast< bool >( replayed ) );
	CHECK( replayed->events_read == 2 );
	CHECK( replayed->malformed_lines == 0 );
	CHECK_FALSE( replayed->truncated_tail );
	CHECK( replayed->log.events( ).front( ).kind == "session.start" );

	const auto listed = cli::list_sessions( workspace );
	REQUIRE( static_cast< bool >( listed ) );
	REQUIRE( listed->size( ) == 1 );
	CHECK( listed->front( ).id == session->id );
	CHECK( listed->front( ).size_bytes > 0 );
	CHECK( listed->front( ).started_ms == session->started_ms );

	// `--continue` resolves to the newest session for the workspace, which is this one.
	const auto continued = cli::resolve_session( workspace, { } );
	REQUIRE( static_cast< bool >( continued ) );
	CHECK( continued->id == session->id );

	// a workspace with no session yet is an empty listing, not an error.
	const auto empty = cli::list_sessions( test::scratch_directory( "mcode-session-none" ) );
	REQUIRE( static_cast< bool >( empty ) );
	CHECK( empty->empty( ) );

	// The reopen path a resume takes: the same file, with the recorded sequence kept.
	{
		auto reopened = event_log{ };
		REQUIRE( static_cast< bool >( reopened.open( session->path ) ) );
		CHECK( reopened.size( ) == 2 );
		CHECK( reopened.next_sequence( ) == 2 );

		// the resume marker is appended after the restored events, not renumbered over them.
		cli::start_session_log( reopened, *session, true );
		CHECK( reopened.size( ) == 3 );
		CHECK( reopened.events( ).back( ).kind == "session.resume" );
		CHECK( reopened.events( ).back( ).sequence == 2 );
	}

	auto resumed_again = replay_event_log( session->path );
	REQUIRE( static_cast< bool >( resumed_again ) );
	CHECK( resumed_again->events_read == 3 );
	CHECK( resumed_again->malformed_lines == 0 );

	std::filesystem::remove( session->path );
	std::filesystem::remove_all( workspace );
}

TEST_CASE( "the session flags parse like every other value flag", "[cli][session]" ) {
	// `--continue` and its short form carry no value.
	auto continued = cli::parse_exec_options( { "--continue", "do it" } );
	REQUIRE( static_cast< bool >( continued ) );
	CHECK( continued->continue_session );
	CHECK( continued->resume_session.empty( ) );
	CHECK_FALSE( continued->list_sessions );
	CHECK( continued->prompt == "do it" );
	CHECK( continued->unknown_arguments.empty( ) );

	auto short_continue = cli::parse_exec_options( { "-c" } );
	REQUIRE( static_cast< bool >( short_continue ) );
	CHECK( short_continue->continue_session );

	// `--resume` consumes the next argument, exactly as `--model` does.
	auto resumed = cli::parse_exec_options( { "--resume", "abc123", "do it" } );
	REQUIRE( static_cast< bool >( resumed ) );
	CHECK( resumed->resume_session == "abc123" );
	CHECK( resumed->prompt == "do it" );
	CHECK( resumed->unknown_arguments.empty( ) );

	auto short_resume = cli::parse_exec_options( { "-r", "abc123" } );
	REQUIRE( static_cast< bool >( short_resume ) );
	CHECK( short_resume->resume_session == "abc123" );

	auto sessions = cli::parse_exec_options( { "--sessions" } );
	REQUIRE( static_cast< bool >( sessions ) );
	CHECK( sessions->list_sessions );
	CHECK_FALSE( sessions->continue_session );

	// `--resume` needs a value, and the missing value is a usage error rather than
	// an unknown-argument report.
	CHECK_FALSE( static_cast< bool >( cli::parse_exec_options( { "--resume" } ) ) );
	CHECK_FALSE( static_cast< bool >( cli::parse_exec_options( { "-r" } ) ) );

	// an explicit id wins over --continue in either order, so the two cannot disagree
	// about which session a run reopens.
	auto both = cli::parse_exec_options( { "--continue", "--resume", "abc123" } );
	REQUIRE( static_cast< bool >( both ) );
	CHECK( both->resume_session == "abc123" );
	CHECK_FALSE( both->continue_session );

	auto reversed = cli::parse_exec_options( { "--resume", "abc123", "--continue" } );
	REQUIRE( static_cast< bool >( reversed ) );
	CHECK( reversed->resume_session == "abc123" );
	CHECK_FALSE( reversed->continue_session );

	// the new flags do not disturb the neighbours, and an unknown flag still lands in
	// unknown_arguments rather than being dropped.
	auto mixed = cli::parse_exec_options( { "--json", "--continue", "--model", "m" } );
	REQUIRE( static_cast< bool >( mixed ) );
	CHECK( mixed->json );
	CHECK( mixed->continue_session );
	CHECK( mixed->model == "m" );

	auto unknown = cli::parse_exec_options( { "--resum", "abc123" } );
	REQUIRE( static_cast< bool >( unknown ) );
	REQUIRE( unknown->unknown_arguments.size( ) == 1 );
	CHECK( unknown->unknown_arguments.front( ) == "--resum" );
	CHECK( unknown->resume_session.empty( ) );

	// the flags are documented, since usage_text is the only place a user learns them.
	const auto usage = cli::usage_text( "mcode" );
	CHECK( usage.find( "--continue" ) != std::string::npos );
	CHECK( usage.find( "--resume" ) != std::string::npos );
	CHECK( usage.find( "--sessions" ) != std::string::npos );
}
