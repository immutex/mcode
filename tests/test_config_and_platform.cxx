#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "mcode/platform/seams.hxx"
#include "mcode/support/config.hxx"
#include "mcode/support/toml.hxx"

using namespace mcode;

namespace {

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

	// A `[table]` header and a dotted key produce the same flattened key, which is
	// what makes lookup uniform.
	REQUIRE( parsed->get_string( "stream.usage.in" ) == std::string{ "/usage/prompt_tokens" } );
	REQUIRE( parsed->contains( "stream.usage.out" ) );

	// Absence is distinguishable from an error.
	REQUIRE_FALSE( parsed->contains( "nope" ) );
	REQUIRE_FALSE( parsed->optional_string( "nope" ).has_value( ) );
}

TEST_CASE( "toml refuses what it cannot represent rather than ignoring it", "[toml]" ) {
	// The failure mode this prevents: a config key that is silently dropped, so
	// the user believes a setting took effect when it did not.
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

	// The line number is the difference between a fixable error and a hunt.
	REQUIRE( parsed.error( ).msg.find( "line 2" ) != std::string::npos );
}

TEST_CASE( "project scope may only add restrictions", "[config]" ) {
	// This is the security-relevant rule: a cloned repository must not be able to
	// widen a permission or disable a sandbox by shipping a config file.
	auto root = std::filesystem::temp_directory_path( ) / "mcode-config-test" / "project";
	std::filesystem::remove_all( root );

	// A project file that only restricts is accepted.
	write_file( root / "ok.toml", R"(
[permissions.deny]
"rm -rf" = "always"
)" );

	auto accepted = config::load_layer( config::scope::project, root / "ok.toml" );
	REQUIRE( static_cast< bool >( accepted ) );

	// Anything else is refused, and the message says why.
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
	auto root = std::filesystem::temp_directory_path( ) / "mcode-config-test" / "user";
	std::filesystem::remove_all( root );

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
	layers.push_back( { .level = config::scope::user, .values = std::move( *user ) } );
	layers.push_back( { .level = config::scope::project, .values = std::move( *project ) } );

	auto merged = config::merged_config::merge( std::move( layers ) );
	REQUIRE( static_cast< bool >( merged ) );

	REQUIRE( merged->get_string( "model" ) == std::optional< std::string >{ "user-model" } );

	// Lists append rather than replace. Overriding wholesale would let a project
	// file's extra deny rule erase the user's, which is the opposite of "may only
	// add".
	auto deny = merged->get_string_array( "deny" );
	REQUIRE( deny.size( ) == 2 );
	REQUIRE( deny[ 0 ] == "a" );
	REQUIRE( deny[ 1 ] == "b" );

	// And the merge records which scope supplied a value, so "why is this set"
	// is answerable.
	REQUIRE( merged->source_of( "model" ) == std::optional< config::scope >{ config::scope::user } );
	REQUIRE( merged->source_of( "deny" ) == std::optional< config::scope >{ config::scope::project } );
}

TEST_CASE( "merge is independent of layer order", "[config]" ) {
	auto low = toml::parse( "a = 1\nlist = [\"low\"]\n" );
	auto high = toml::parse( "a = 2\nlist = [\"high\"]\n" );
	REQUIRE( static_cast< bool >( low ) );
	REQUIRE( static_cast< bool >( high ) );

	auto forward = std::vector< config::layer >{ };
	forward.push_back( { .level = config::scope::user, .values = std::move( *low ) } );
	forward.push_back( { .level = config::scope::project, .values = std::move( *high ) } );

	auto merged = config::merged_config::merge( std::move( forward ) );
	REQUIRE( static_cast< bool >( merged ) );
	REQUIRE( merged->get_int( "a" ) == std::optional< std::int64_t >{ 2 } );
}

TEST_CASE( "the seven platform seams exist and report honestly", "[platform]" ) {
	// E1's acceptance: interfaces fixed on all three platforms. What is testable
	// here is that every seam answers, and that the ones M0 does not implement say
	// so rather than pretending.

	// 1. PtySession
	REQUIRE( platform::pty_session::supported( ) );
	auto pty = platform::pty_session::spawn( "sh", { } );
	REQUIRE_FALSE( static_cast< bool >( pty ) );
	REQUIRE( pty.error( ).code == errc::unsupported );

	// 2. Sandbox: not enforced yet, and the level says so rather than the caller
	// assuming isolation.
	REQUIRE( platform::sandbox_support_level( ) == platform::sandbox_support::unavailable );
	REQUIRE_FALSE( platform::sandbox_mechanism( ).empty( ) );

	auto applied = platform::apply_sandbox( { } );
	REQUIRE_FALSE( static_cast< bool >( applied ) );
	REQUIRE( applied.error( ).code == errc::unsupported );

	// 3. Termination
	REQUIRE( platform::process_is_alive( 0xFFFFFFFFu ) == false );

	// 4. fs helpers
	auto canonical = platform::canonicalize( std::filesystem::current_path( ) );
	REQUIRE( static_cast< bool >( canonical ) );

	auto temp = platform::temp_directory( );
	REQUIRE( static_cast< bool >( temp ) );
	REQUIRE( std::filesystem::exists( *temp ) );

#if defined( _WIN32 )
	REQUIRE( platform::case_insensitive_paths( ) );
	auto extended = platform::to_extended_path( "C:\\Windows" );
	REQUIRE( extended.wstring( ).starts_with( L"\\\\?\\" ) );
#else
	REQUIRE_FALSE( platform::case_insensitive_paths( ) );
#endif

	// 5. paths
	auto config_path = platform::app_data_path( platform::data_kind::config );
	REQUIRE( static_cast< bool >( config_path ) );
	REQUIRE( config_path->string( ).find( "mcode" ) != std::string::npos );

	auto cache_path = platform::app_data_path( platform::data_kind::cache );
	REQUIRE( static_cast< bool >( cache_path ) );

	// 6. ResizeSource
	auto size = platform::terminal_size( );
	if ( size ) {
		REQUIRE( size->first > 0 );
		REQUIRE( size->second > 0 );
	}

	// 7. FileWatch: deliberately not a core primitive.
	REQUIRE_FALSE( platform::file_watcher::supported( ) );
	auto watcher = platform::file_watcher::create( *temp, false );
	REQUIRE_FALSE( static_cast< bool >( watcher ) );
	REQUIRE( watcher.error( ).code == errc::unsupported );
}
