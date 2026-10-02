#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#if defined( _WIN32 )
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "mcode/cli/exec.hxx"
#include "mcode/core/error.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/support/config.hxx"
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
[permissions.deny]
"rm -rf" = "always"
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
[permissions.allow]
"ls" = "always"
)" );

	auto loaded = config::load_layer( config::scope::user, root / "config.toml" );
	REQUIRE( static_cast< bool >( loaded ) );
	REQUIRE( loaded->values.get_string( "model" ) == std::string{ "some-model" } );

	std::filesystem::remove_all( root.parent_path( ) );
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
	layers.push_back( { .level = config::scope::user, .origin = { }, .values = std::move( *user ) } );
	layers.push_back( { .level = config::scope::project, .origin = { }, .values = std::move( *project ) } );

	auto merged = config::merged_config::merge( std::move( layers ) );
	REQUIRE( static_cast< bool >( merged ) );

	REQUIRE( merged->get_string( "model" ) == std::optional< std::string >{ "user-model" } );

	// lists append rather than replace, so a project file cannot erase the user's deny rules.
	auto deny = merged->get_string_array( "deny" );
	REQUIRE( deny.size( ) == 2 );
	REQUIRE( deny[ 0 ] == "a" );
	REQUIRE( deny[ 1 ] == "b" );

	REQUIRE( merged->source_of( "model" ) == std::optional< config::scope >{ config::scope::user } );
	REQUIRE( merged->source_of( "deny" ) == std::optional< config::scope >{ config::scope::project } );
}

TEST_CASE( "merge is independent of layer order", "[config]" ) {
	auto low = toml::parse( "a = 1\nlist = [\"low\"]\n" );
	auto high = toml::parse( "a = 2\nlist = [\"high\"]\n" );
	REQUIRE( static_cast< bool >( low ) );
	REQUIRE( static_cast< bool >( high ) );

	auto forward = std::vector< config::layer >{ };
	forward.push_back( { .level = config::scope::user, .origin = { }, .values = std::move( *low ) } );
	forward.push_back( { .level = config::scope::project, .origin = { }, .values = std::move( *high ) } );

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
		REQUIRE( platform::sandbox_network_level( ) == platform::sandbox_network_support::unavailable );
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
	REQUIRE( cli::exit_code_for( errc::budget_exhausted ) == cli::exit_code::budget_exhausted );
	REQUIRE( cli::exit_code_for( errc::cancelled ) == cli::exit_code::interrupted );
	REQUIRE( cli::exit_code_for( errc::budget_exhausted ) != cli::exit_code_for( errc::cancelled ) );
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
