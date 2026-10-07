#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined( _WIN32 )
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/config.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/parse.hxx"
#include "mcode/support/toml.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	// ctest redirects stdout, so CI only exercises the negative branch.
	auto terminal_attached( ) -> bool {
	#if defined( _WIN32 )
		return _isatty( _fileno( stdout ) ) != 0;
	#else
		return ::isatty( STDOUT_FILENO ) != 0;
	#endif
	}

	auto write_file( const std::filesystem::path& path, const std::string_view text ) -> void {
		std::filesystem::create_directories( path.parent_path( ) );

		auto stream = std::ofstream{ path, std::ios::binary | std::ios::trunc };
		stream << text;
	}

}

TEST_CASE( "toml parses the subset config and manifests use", "[toml]" ) {
	auto parsed = toml::parse( R"(
# a comment
name = "git"
version = "0.3.0"
api_version = 1
enabled = true
timeout_ms = 5000
ratio = 0.25
permissions = ["fs_read", "spawn"]

[stream.usage]
in = "/usage/prompt_tokens"
out = "/usage/completion_tokens"
)" );

	REQUIRE( static_cast< bool >( parsed ) );

	REQUIRE( parsed->get_string( "name" ) == std::string{ "git" } );
	REQUIRE( parsed->get_int( "api_version" ) == 1 );
	REQUIRE( parsed->get_bool( "enabled" ) == true );

	auto permissions = parsed->get_string_array( "permissions" );
	REQUIRE( static_cast< bool >( permissions ) );
	REQUIRE( permissions->size( ) == 2 );
	REQUIRE( ( *permissions )[ 0 ] == "fs_read" );

	// a `[table]` header and a dotted key flatten to the same key.
	REQUIRE( parsed->get_string( "stream.usage.in" ) == std::string{ "/usage/prompt_tokens" } );
	REQUIRE( parsed->contains( "stream.usage.out" ) );

	// absence, a present value, and a wrong type are three different answers.
	REQUIRE_FALSE( parsed->contains( "nope" ) );

	auto absent = parsed->optional_string( "nope" );
	REQUIRE( static_cast< bool >( absent ) );

	if ( absent ) {
		REQUIRE_FALSE( absent->has_value( ) );
	}

	auto present = parsed->optional_string( "name" );
	REQUIRE( static_cast< bool >( present ) );

	if ( present ) {
		REQUIRE( present->has_value( ) );
	}

	// a wrong type is an error, not an absence.
	auto mistyped = parsed->optional_int( "version" );
	REQUIRE_FALSE( static_cast< bool >( mistyped ) );

	if ( !mistyped ) {
		REQUIRE( mistyped.error( ).msg.find( "not an integer" ) != std::string::npos );
	}
}

TEST_CASE( "toml refuses what it cannot represent rather than ignoring it", "[toml]" ) {
	const auto cases = std::vector< std::pair< const char*, const char* > >{
		{ R"(when = 1979-05-27T07:32:00Z)", "date" },
		{ R"(point = { x = 1, y = 2 })", "inline table" },
		{ R"(text = """multi""")", "multi-line string" },
		{ R"(bad = "unterminated)", "unterminated string" },
		{ R"(name = )", "missing value" },
		{ R"(name)", "no equals" },
		{ R"(name = "a"
name = "b")", "duplicate key" },
		{ R"(mixed = ["a", 1])", "heterogeneous array" },
		{ R"(bad = "\q")", "unsupported escape" },
		{ R"([unclosed)", "unterminated table header" },
		{ R"(name = "a" trailing)", "text after a value" },

		// a malformed number must be refused, not truncated to its numeric prefix.
		{ R"(ratio = 0.25.9)", "a number with two dots" },
		{ R"(ratio = 1e)", "an exponent with no digits" },
		{ R"(ratio = 1.2e5.6)", "an exponent followed by more digits" },
	};

	for ( const auto& [ text, label ] : cases ) {
		auto parsed = toml::parse( text );

		CHECK_FALSE( static_cast< bool >( parsed ) );

		if ( parsed ) {
			FAIL( "case '" << label << "' parsed but should not have" );
		}
	}
}

TEST_CASE( "a parse error names the line", "[toml]" ) {
	auto parsed = toml::parse( "ok = 1\nok = 2\n" );
	REQUIRE_FALSE( static_cast< bool >( parsed ) );

	REQUIRE( parsed.error( ).msg.find( "line 2" ) != std::string::npos );
}

TEST_CASE( "project scope may only add restrictions", "[config]" ) {
	// a cloned repository must not be able to widen a permission or disable a sandbox.
	auto root = test::scratch_directory( "mcode-config-test" ) / "project";

	write_file( root / "ok.toml", R"(
permissions.deny = ["rm -rf"]
)" );

	auto accepted = config::load_layer( config::scope::project, root / "ok.toml" );
	REQUIRE( static_cast< bool >( accepted ) );

	const auto forbidden = std::vector< std::pair< const char*, const char* > >{
		{ R"(sandbox = "off")", "sandbox" },
		{ R"(model = "gpt-9")", "model" },
		{ R"([permissions.allow]
"rm" = "always")", "allow" },
		{ R"(extensions_dir = "/tmp/evil")", "extensions_dir" },
		{ R"(api_key = "stolen")", "api_key" },
	};

	for ( const auto& [ text, label ] : forbidden ) {
		write_file( root / "bad.toml", text );

		auto refused = config::load_layer( config::scope::project, root / "bad.toml" );

		CHECK_FALSE( static_cast< bool >( refused ) );

		if ( refused ) {
			FAIL( "project scope accepted '" << label << "'" );
		}

		REQUIRE( refused.error( ).msg.find( "project scope may not set" ) != std::string::npos );
	}

	std::filesystem::remove_all( root.parent_path( ) );
}

TEST_CASE( "user scope may set anything the project may not", "[config]" ) {
	auto root = test::scratch_directory( "mcode-config-test" ) / "user";

	write_file( root / "config.toml", R"(
model = "some-model"
sandbox = "enforce"
permissions.deny = ["ls"]
)" );

	auto loaded = config::load_layer( config::scope::user, root / "config.toml" );
	REQUIRE( static_cast< bool >( loaded ) );
	REQUIRE( loaded->values.get_string( "model" ) == std::string{ "some-model" } );

	std::filesystem::remove_all( root.parent_path( ) );
}

TEST_CASE( "a deeper permissions key is refused, never silently dropped", "[config]" ) {
	auto root = test::scratch_directory( "mcode-config-deep" ) / "user";

	write_file( root / "config.toml", R"(
[permissions.deny]
"rm -rf" = "always"
)" );

	auto refused = config::load_layer( config::scope::user, root / "config.toml" );
	CHECK_FALSE( static_cast< bool >( refused ) );

	if ( refused ) {
		return;
	}

	CHECK( refused.error( ).msg.find( "permissions.deny" ) != std::string::npos );

	std::filesystem::remove_all( root.parent_path( ) );
}

TEST_CASE( "a scalar permission rule is refused, never an empty list", "[config]" ) {
	auto root = test::scratch_directory( "mcode-config-scalar" ) / "user";

	write_file( root / "config.toml", R"(
permissions.deny = "rm -rf /"
)" );

	auto refused = config::load_layer( config::scope::user, root / "config.toml" );
	CHECK_FALSE( static_cast< bool >( refused ) );

	std::filesystem::remove_all( root.parent_path( ) );

	// the accessor itself is fail-closed too, for a value that reached a merged config
	auto user = toml::parse( "deny = \"rm -rf /\"\n" );
	REQUIRE( static_cast< bool >( user ) );

	auto layers = std::vector< config::layer >{ };
	layers.push_back( { .level = config::scope::user, .origin = { },
		.values = std::move( *user ) } );

	auto merged = config::merged_config::merge( std::move( layers ) );
	REQUIRE( static_cast< bool >( merged ) );

	CHECK_FALSE( static_cast< bool >( merged->get_string_array( "deny" ) ) );
}

TEST_CASE( "later scopes override scalars and merge arrays", "[config]" ) {
	auto user = toml::parse( R"(
model = "user-model"
timeout = 10
deny = ["a"]
)" );

	auto project = toml::parse( R"(
deny = ["b"]
)" );

	REQUIRE( static_cast< bool >( user ) );
	REQUIRE( static_cast< bool >( project ) );

	auto layers = std::vector< config::layer >{ };
	layers.push_back( { .level = config::scope::user, .origin = { },
		.values = std::move( *user ) } );
	layers.push_back( { .level = config::scope::project, .origin = { },
		.values = std::move( *project ) } );

	auto merged = config::merged_config::merge( std::move( layers ) );
	REQUIRE( static_cast< bool >( merged ) );

	REQUIRE( merged->get_string( "model" ) == std::optional< std::string >{ "user-model" } );

	// lists append rather than replace, so a project file cannot erase the user's deny rules.
	auto deny = merged->get_string_array( "deny" );
	REQUIRE( static_cast< bool >( deny ) );
	REQUIRE( deny->size( ) == 2 );
	REQUIRE( ( *deny )[ 0 ] == "a" );
	REQUIRE( ( *deny )[ 1 ] == "b" );

	REQUIRE( merged->source_of( "model" )
		== std::optional< config::scope >{ config::scope::user } );
	REQUIRE( merged->source_of( "deny" )
		== std::optional< config::scope >{ config::scope::project } );
}

TEST_CASE( "merge is independent of layer order", "[config]" ) {
	auto low = toml::parse( "a = 1\nlist = [\"low\"]\n" );
	auto high = toml::parse( "a = 2\nlist = [\"high\"]\n" );
	REQUIRE( static_cast< bool >( low ) );
	REQUIRE( static_cast< bool >( high ) );

	auto forward = std::vector< config::layer >{ };
	forward.push_back( { .level = config::scope::user, .origin = { },
		.values = std::move( *low ) } );
	forward.push_back( { .level = config::scope::project, .origin = { },
		.values = std::move( *high ) } );

	auto merged = config::merged_config::merge( std::move( forward ) );
	REQUIRE( static_cast< bool >( merged ) );
	REQUIRE( merged->get_int( "a" ) == std::optional< std::int64_t >{ 2 } );
}

TEST_CASE( "the seven platform seams exist and report honestly", "[platform]" ) {
	const auto pty_supported = platform::pty_session::supported( );

	auto pty = platform::pty_session::spawn( "sh", { } );

	if ( pty_supported ) {
		REQUIRE( static_cast< bool >( pty ) );
	} else {
		REQUIRE_FALSE( static_cast< bool >( pty ) );

		if ( !pty ) {
			REQUIRE( pty.error( ).code == errc::unsupported );
		}
	}

	// the seam reports per capability, not one merged claim.
	REQUIRE_FALSE( platform::sandbox_mechanism( ).empty( ) );

	// apply_sandbox is not called here: on POSIX it confines the calling process.
	if ( platform::sandbox_capability_level( ) == platform::sandbox_capability::unavailable ) {
		REQUIRE( platform::sandbox_network_level( )
			== platform::sandbox_network_support::unavailable );
	}

	// on POSIX `kill(0, 0)` and `kill(-1, 0)` succeed, so an unvalidated pid reports as alive.
	REQUIRE( platform::process_is_alive( 0 ) == false );
	REQUIRE( platform::process_is_alive( 0xFFFFFFFFu ) == false );
	REQUIRE( platform::process_is_alive( 0x80000000u ) == false );

#if defined( _WIN32 )
	REQUIRE( platform::process_is_alive( ::GetCurrentProcessId( ) ) );
#else
	REQUIRE( platform::process_is_alive( static_cast< std::uint64_t >( ::getpid( ) ) ) );
#endif

	auto canonical = platform::canonicalize( std::filesystem::current_path( ) );
	REQUIRE( static_cast< bool >( canonical ) );

	auto temp = platform::temp_directory( );
	REQUIRE( static_cast< bool >( temp ) );
	REQUIRE( std::filesystem::exists( *temp ) );

#if defined( _WIN32 ) || defined( __APPLE__ )
	// true even on a case-sensitive volume: the boundary compares case-folded.
	REQUIRE( platform::case_insensitive_paths( ) );
#endif

#if defined( _WIN32 )
	auto extended = platform::to_extended_path( "C:\\Windows" );
	REQUIRE( extended.wstring( ).starts_with( L"\\\\?\\" ) );
#endif

#if !defined( _WIN32 ) && !defined( __APPLE__ )
	REQUIRE_FALSE( platform::case_insensitive_paths( ) );
#endif

	auto config_path = platform::app_data_path( platform::data_kind::config );
	REQUIRE( static_cast< bool >( config_path ) );
	REQUIRE( config_path->string( ).find( "mcode" ) != std::string::npos );

	auto cache_path = platform::app_data_path( platform::data_kind::cache );
	REQUIRE( static_cast< bool >( cache_path ) );

	// a size is reported only when there is a terminal, and it is positive.
	auto size = platform::terminal_size( );

	if ( terminal_attached( ) ) {
		REQUIRE( static_cast< bool >( size ) );

		if ( size ) {
			REQUIRE( size->first > 0 );
			REQUIRE( size->second > 0 );
		}
	} else {
		REQUIRE_FALSE( static_cast< bool >( size ) );
	}

	REQUIRE_FALSE( platform::file_watcher::supported( ) );
	auto watcher = platform::file_watcher::create( *temp, false );
	REQUIRE_FALSE( static_cast< bool >( watcher ) );
	REQUIRE( watcher.error( ).code == errc::unsupported );
}

TEST_CASE( "every exit code is the documented number", "[cli]" ) {
	// these numbers are an interface a CI script branches on.
	REQUIRE( cli::to_int( cli::exit_code::success ) == 0 );
	REQUIRE( cli::to_int( cli::exit_code::verification_failed ) == 1 );
	REQUIRE( cli::to_int( cli::exit_code::usage_error ) == 2 );
	REQUIRE( cli::to_int( cli::exit_code::budget_exhausted ) == 3 );
	REQUIRE( cli::to_int( cli::exit_code::provider_error ) == 4 );
	REQUIRE( cli::to_int( cli::exit_code::permission_denied ) == 5 );
	REQUIRE( cli::to_int( cli::exit_code::interrupted ) == 130 );
}

TEST_CASE( "budget exhaustion is not an interruption", "[cli]" ) {
	REQUIRE( cli::exit_code_for( errc::budget_exhausted )
		== cli::exit_code::budget_exhausted );
	REQUIRE( cli::exit_code_for( errc::cancelled ) == cli::exit_code::interrupted );
	REQUIRE( cli::exit_code_for( errc::budget_exhausted )
		!= cli::exit_code_for( errc::cancelled ) );
}

TEST_CASE( "the error-to-exit mapping is total", "[cli]" ) {
	// the mapping must be total: an unmapped error silently becomes 1, "verification failed".
	const auto codes = std::array{
		errc::ok, errc::io, errc::json, errc::protocol, errc::tool_failed,
		errc::cancelled, errc::budget_exhausted, errc::lua_error, errc::config,
		errc::unsupported,
	};

	for ( const auto code : codes ) {
		REQUIRE_FALSE( to_string( code ).empty( ) );
	}

	for ( const auto code : codes ) {
		if ( code == errc::ok ) {
			continue;
		}

		REQUIRE( cli::exit_code_for( code ) != cli::exit_code::success );
	}
}

TEST_CASE( "a handoff that is neither a budget stop nor a denial still fails", "[cli]" ) {
	auto completion = turn_outcome{ };
	completion.final_state = loop_state::handoff;

	// An unverified turn ends in handoff with no recorded reason: that is a completion.
	REQUIRE( cli::exit_code_for_run( completion, false, false ) == cli::exit_code::success );

	// A failed model call or a guard trip hands off with the reason recorded.
	auto gave_up = completion;
	gave_up.summary_json = R"({"reason":"thrash beyond replan guard"})";

	REQUIRE( cli::exit_code_for_run( gave_up, false, false ) == cli::exit_code::provider_error );

	// The budget and the denial stay more specific than the give-up reason.
	REQUIRE( cli::exit_code_for_run( gave_up, true, false )
		== cli::exit_code::budget_exhausted );
	REQUIRE( cli::exit_code_for_run( gave_up, false, true )
		== cli::exit_code::permission_denied );

	auto failed = turn_outcome{ };
	failed.final_state = loop_state::failed;

	REQUIRE( cli::exit_code_for_run( failed, false, false ) == cli::exit_code::provider_error );

	auto done = turn_outcome{ };
	done.final_state = loop_state::done;

	REQUIRE( cli::exit_code_for_run( done, false, false ) == cli::exit_code::success );
}

TEST_CASE( "a UTF-8 BOM does not make the file unreadable", "[toml]" ) {
	// notepad writes a UTF-8 BOM; without the skip the first byte is not a space, a '#', or a '['.
	auto parsed = toml::parse( "\xEF\xBB\xBFmodel = \"gpt\"\n" );

	REQUIRE( static_cast< bool >( parsed ) );

	if ( !parsed ) {
		FAIL( parsed.error( ).msg );
	}

	auto model = parsed->get_string( "model" );
	REQUIRE( model.has_value( ) );

	if ( model ) {
		REQUIRE( *model == "gpt" );
	}

	// a mid-file BOM is content, not a marker; concatenated because `\xBFbad` is one hex escape.
	const auto bom_mid_file = std::string{ "ok = 1\n" } + "\xEF\xBB\xBF" + "bad = 2\n";

	CHECK_FALSE( static_cast< bool >( toml::parse( bom_mid_file ) ) );
}

TEST_CASE( "the float parser accepts and refuses exactly what it says", "[toml]" ) {
	// the compiler's float from_chars is missing on Xcode 15, so this parser is the implementation.
	auto value = 0.0;

	for ( const auto& [ text, expected ] : std::vector< std::pair< const char*, double > >{
		{ "0", 0.0 },
		{ "1", 1.0 },
		{ "-1", -1.0 },
		{ "+2", 2.0 },
		{ "0.25", 0.25 },
		{ "-0.25", -0.25 },
		{ "1.5e3", 1500.0 },
		{ "1.5E3", 1500.0 },
		{ "1e-3", 0.001 },
		{ "1.5e+3", 1500.0 },
		{ "123456789012345678", 123456789012345678.0 },
		{ "0.0", 0.0 },
		{ "100", 100.0 },
	} ) {
		CHECK( support::parse_double( text, value ) );
		CHECK( value == expected );
	}

	for ( const auto* text : {
		"", "+", "-", ".", ".5", "5.", "e5", "1e", "1e+", "1E-",
		"0.25.9", "1.2e5.6", "1.2.3", "abc", "1a", "1 ", " 1", "1_000",
		"0x10", "inf", "nan", "--1", "1..2", "1e99999", "1e-99999",
		"1234567890123456789", "1.7976931348623157e309",
	} ) {
		CHECK_FALSE( support::parse_double( text, value ) );
	}

	// underflow to zero is refused, not reported as 0.
	CHECK_FALSE( support::parse_double( "1e-400", value ) );
}

TEST_CASE( "the float parser rounds once, not per decade", "[toml]" ) {
	auto value = 0.0;

	REQUIRE( support::parse_double( "3.14", value ) );
	CHECK( value == 3.14 );

	REQUIRE( support::parse_double( "1e100", value ) );
	CHECK( value == 1e100 );

	REQUIRE( support::parse_double( "1.7976931348623157e308", value ) );
	CHECK( value == 1.7976931348623157e308 );

	// DBL_MIN is a normal double even though 10^-324 itself underflows
	REQUIRE( support::parse_double( "2.2250738585072014e-308", value ) );
	CHECK( value == 2.2250738585072014e-308 );

	// a zero significand stays zero whatever the exponent
	REQUIRE( support::parse_double( "0e400", value ) );
	CHECK( value == 0.0 );
}

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

	// --continue with no session at all is an error too, never a silent fresh start.
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

	// --continue resolves to the newest session for the workspace, which is this one.
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
	// --continue and its short form, which carry no value.
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

	// --resume consumes the next argument, exactly as --model does.
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

	// --resume needs a value, and the missing value is a usage error rather than an
	// unknown-argument report.
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

TEST_CASE( "the worktree flag takes an optional name and never swallows the next flag",
	"[cli][worktree]" ) {
	// No name: the next token is a flag, so it must not be consumed as a branch label.
	auto bare = cli::parse_exec_options( { "--worktree", "--json" } );
	REQUIRE( static_cast< bool >( bare ) );
	CHECK( bare->worktree );
	CHECK( bare->worktree_name.empty( ) );
	CHECK( bare->json );

	// Short form, with a name.
	auto named = cli::parse_exec_options( { "-w", "feature-x" } );
	REQUIRE( static_cast< bool >( named ) );
	CHECK( named->worktree );
	CHECK( named->worktree_name == "feature-x" );

	// Long form with a name.
	auto long_named = cli::parse_exec_options( { "--worktree", "probe" } );
	REQUIRE( static_cast< bool >( long_named ) );
	CHECK( long_named->worktree_name == "probe" );

	// Off unless asked for.
	auto plain = cli::parse_exec_options( { "do the thing" } );
	REQUIRE( static_cast< bool >( plain ) );
	CHECK_FALSE( plain->worktree );
}
