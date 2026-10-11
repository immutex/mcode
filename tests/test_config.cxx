#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/support/config.hxx"
#include "mcode/support/toml.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	auto write_file( const std::filesystem::path& path, const std::string_view text ) -> void {
		std::filesystem::create_directories( path.parent_path( ) );

		auto stream = std::ofstream{ path, std::ios::binary | std::ios::trunc };
		stream << text;
	}

}

TEST_CASE( "project scope may only add restrictions", "[config]" ) {
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
