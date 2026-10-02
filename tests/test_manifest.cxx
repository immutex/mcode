#include "ext_test_helpers.hxx"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace mcode;
using namespace ext_test;

TEST_CASE( "a valid manifest loads", "[loader]" ) {
	auto manifest = ext::load_manifest( extensions_root( ) / "hello-tool" );

	REQUIRE( static_cast< bool >( manifest ) );

	if ( !manifest ) {
		FAIL( manifest.error( ).msg );
	}

	REQUIRE( manifest->name == "hello-tool" );
	REQUIRE( manifest->version == "0.1.0" );
	REQUIRE( manifest->api_version == 1 );
	REQUIRE( manifest->has_permission( "fs_read" ) );
	REQUIRE_FALSE( manifest->has_permission( "fs_write" ) );
	REQUIRE_FALSE( manifest->has_permission( "net" ) );
}

TEST_CASE( "an unknown manifest key is rejected", "[loader]" ) {
	auto manifest = ext::load_manifest( extensions_root( ) / "broken-manifest" );

	REQUIRE_FALSE( static_cast< bool >( manifest ) );
	REQUIRE( manifest.error( ).msg.find( "unknown key" ) != std::string::npos );
	REQUIRE( manifest.error( ).msg.find( "permission" ) != std::string::npos );
}

TEST_CASE( "manifest validation rejects what would fail later", "[loader]" ) {
	const auto root = scratch_root( );

	const auto cases = std::vector< std::pair< const char*, const char* > >{
		{ R"(version = "0.1.0"
api_version = 1)", "no name" },
		{ R"(name = "bad"
version = "0.1.0"
api_version = 1)", "name does not match the directory" },
		{ R"(name = "bad-name"
api_version = 1)", "no version" },
		{ R"(name = "bad-name"
version = "not-a-version"
api_version = 1)", "bad version" },
		{ R"(name = "bad-name"
version = "0.1.0")", "no api_version" },
		{ R"(name = "bad-name"
version = "0.1.0"
api_version = ">=1")", "api_version is a range, not an integer" },
		{ R"(name = "bad-name"
version = "0.1.0"
api_version = 99)", "api_version above this build" },
		{ R"(name = "bad-name"
version = "0.1.0"
api_version = 1
permissions = ["fs_reed"])", "unknown permission" },
	};

	for ( const auto& [ text, label ] : cases ) {
		write( root / "bad-name" / "ext.toml", text );

		auto manifest = ext::load_manifest( root / "bad-name" );

		CHECK_FALSE( static_cast< bool >( manifest ) );

		if ( manifest ) {
			FAIL( "case '" << label << "' was accepted but should not be" );
		}
	}

	std::filesystem::remove_all( root );
}

TEST_CASE( "a name that is not lowercase-kebab-case is refused", "[loader]" ) {
	const auto root = scratch_root( );

	for ( const auto* name : { "Bad", "bad_name", "-bad", "bad-", "bad--name" } ) {
		write( root / name / "ext.toml",
			std::string{ "name = \"" } + name + "\"\nversion = \"0.1.0\"\napi_version = 1\n" );

		auto manifest = ext::load_manifest( root / name );

		CHECK_FALSE( static_cast< bool >( manifest ) );

		if ( manifest ) {
			FAIL( "the name '" << name << "' was accepted" );
		}

		CHECK( manifest.error( ).msg.find( "invalid name" ) != std::string::npos );
	}

	write( root / "badname" / "ext.toml",
		"name = \"bad name\"\nversion = \"0.1.0\"\napi_version = 1\n" );

	auto spaced = ext::load_manifest( root / "badname" );
	CHECK_FALSE( static_cast< bool >( spaced ) );

	std::filesystem::remove_all( root );
}

TEST_CASE( "a version that is not MAJOR.MINOR.PATCH is refused", "[loader]" ) {
	const auto root = scratch_root( );

	for ( const auto* version : { "0.1.0", "1.0.0", "10.20.30", "1.0.0-beta", "1.0.0-rc.1",
		"1.0.0+build.5" } ) {
		write( root / "versions" / "ext.toml",
			std::string{ "name = \"versions\"\nversion = \"" } + version + "\"\napi_version = 1\n" );

		auto manifest = ext::load_manifest( root / "versions" );

		CHECK( static_cast< bool >( manifest ) );

		if ( !manifest ) {
			FAIL( "version '" << version << "' was refused: " << manifest.error( ).msg );
		}
	}

	for ( const auto* version : { "1", "1.2", "1.2.3.4", "v1.2.3", "1..2", "1.2.", "",
		"1.2.3-", "1.2.3+", "1.2.-3", "a.b.c" } ) {
		write( root / "versions" / "ext.toml",
			std::string{ "name = \"versions\"\nversion = \"" } + version + "\"\napi_version = 1\n" );

		auto manifest = ext::load_manifest( root / "versions" );

		CHECK_FALSE( static_cast< bool >( manifest ) );

		if ( manifest ) {
			FAIL( "version '" << version << "' was accepted" );
		}
	}

	// the version bound is checked before the multiply, so overflow is a clean rejection, not UB
	write( root / "versions" / "ext.toml",
		"name = \"versions\"\nversion = \"99999999999999999999.0.0\"\napi_version = 1\n" );
	CHECK_FALSE( static_cast< bool >( ext::load_manifest( root / "versions" ) ) );

	std::filesystem::remove_all( root );
}

TEST_CASE( "a description past the cap is refused", "[loader]" ) {
	// the manifest cap bounds the prompt payload every loading session carries
	const auto root = scratch_root( );

	write( root / "chatty" / "ext.toml",
		std::string{ "name = \"chatty\"\nversion = \"0.1.0\"\napi_version = 1\ndescription = \"" } +
			std::string( 2000, 'x' ) + "\"\n" );

	auto manifest = ext::load_manifest( root / "chatty" );

	REQUIRE_FALSE( static_cast< bool >( manifest ) );

	if ( !manifest ) {
		REQUIRE( manifest.error( ).msg.find( "description" ) != std::string::npos );
	}

	write( root / "chatty" / "ext.toml",
		std::string{ "name = \"chatty\"\nversion = \"0.1.0\"\napi_version = 1\ndescription = \"" } +
			std::string( 1024, 'x' ) + "\"\n" );

	CHECK( static_cast< bool >( ext::load_manifest( root / "chatty" ) ) );

	std::filesystem::remove_all( root );
}
