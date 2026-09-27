#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "mcode/core/registry.hxx"

using mcode::tool_class;
using mcode::tool_def;
using mcode::tool_registry;
using mcode::tool_source;

namespace {

	auto make( std::string name, const tool_source source, std::string owner = { } ) -> tool_def {
		auto definition = tool_def{ };
		definition.name = std::move( name );
		definition.description = "test tool";
		definition.klass = tool_class::read;
		definition.source = source;
		definition.owner = std::move( owner );

		return definition;
	}

}

TEST_CASE( "core tools register and are found by name", "[registry]" ) {
	auto registry = tool_registry{ };
	REQUIRE( registry.add( make( "read", tool_source::core ) ) );
	REQUIRE( registry.add( make( "bash", tool_source::core ) ) );

	CHECK( registry.size( ) == 2 );
	CHECK( registry.find( "read" ) != nullptr );
	CHECK( registry.find( "bash" ) != nullptr );
	CHECK( registry.find( "absent" ) == nullptr );
}

TEST_CASE( "duplicate names are rejected across every source", "[registry]" ) {
	auto registry = tool_registry{ };
	REQUIRE( registry.add( make( "read", tool_source::core ) ) );

	auto clash = registry.add( make( "read", tool_source::bundled_extension, "fs-extra" ) );
	REQUIRE_FALSE( clash );
	CHECK( clash.error( ).code == mcode::errc::config );
	CHECK( registry.size( ) == 1 );

	const auto* existing = registry.find( "read" );
	REQUIRE( existing != nullptr );
	CHECK( existing->source == tool_source::core );
}

TEST_CASE( "non-core tools must name their owner", "[registry]" ) {
	auto registry = tool_registry{ };
	auto anonymous = registry.add( make( "mystery", tool_source::bundled_extension ) );
	REQUIRE_FALSE( anonymous );
	CHECK( anonymous.error( ).code == mcode::errc::config );
}

TEST_CASE( "empty names are refused", "[registry]" ) {
	auto registry = tool_registry{ };
	auto empty = registry.add( make( "", tool_source::core ) );
	REQUIRE_FALSE( empty );
	CHECK( empty.error( ).code == mcode::errc::config );
}

TEST_CASE( "iteration order is sorted, not hash order", "[registry]" ) {
	auto registry = tool_registry{ };

	for ( const auto* name : { "zebra", "alpha", "middle", "beta" } ) {
		REQUIRE( registry.add( make( name, tool_source::core ) ) );
	}

	const auto all = registry.all( );
	REQUIRE( all.size( ) == 4 );

	auto names = std::vector< std::string >{ };

	for ( const auto* definition : all ) {
		names.push_back( definition->name );
	}

	CHECK( names == std::vector< std::string >{ "alpha", "beta", "middle", "zebra" } );
}

TEST_CASE( "ownership queries return one extension's tools sorted", "[registry]" ) {
	auto registry = tool_registry{ };
	REQUIRE( registry.add( make( "read", tool_source::core ) ) );
	REQUIRE( registry.add( make( "todo", tool_source::bundled_extension, "fs-extra" ) ) );
	REQUIRE( registry.add( make( "todo_write", tool_source::bundled_extension, "fs-extra" ) ) );
	REQUIRE( registry.add( make( "web_search", tool_source::bundled_extension, "web" ) ) );

	const auto owned = registry.owned_by( "fs-extra" );
	REQUIRE( owned.size( ) == 2 );
	CHECK( owned[ 0 ]->name == "todo" );
	CHECK( owned[ 1 ]->name == "todo_write" );

	CHECK( registry.owned_by( "web" ).size( ) == 1 );
	CHECK( registry.owned_by( "absent" ).empty( ) );
}

TEST_CASE( "removing an owner removes exactly its tools", "[registry]" ) {
	auto registry = tool_registry{ };
	REQUIRE( registry.add( make( "read", tool_source::core ) ) );
	REQUIRE( registry.add( make( "todo", tool_source::bundled_extension, "fs-extra" ) ) );
	REQUIRE( registry.add( make( "todo_write", tool_source::bundled_extension, "fs-extra" ) ) );
	REQUIRE( registry.add( make( "web_search", tool_source::bundled_extension, "web" ) ) );

	CHECK( registry.remove_owner( "fs-extra" ) == 2 );
	CHECK( registry.size( ) == 2 );
	CHECK( registry.find( "todo" ) == nullptr );
	CHECK( registry.find( "read" ) != nullptr );
	CHECK( registry.find( "web_search" ) != nullptr );
	CHECK( registry.remove_owner( "fs-extra" ) == 0 );
}

TEST_CASE( "an empty owner never mass-removes core tools", "[registry]" ) {
	auto registry = tool_registry{ };
	REQUIRE( registry.add( make( "read", tool_source::core ) ) );
	REQUIRE( registry.add( make( "write", tool_source::core ) ) );

	CHECK( registry.remove_owner( "" ) == 0 );
	CHECK( registry.size( ) == 2 );
}

TEST_CASE( "a removed name can be re-registered", "[registry]" ) {
	auto registry = tool_registry{ };
	REQUIRE( registry.add( make( "todo", tool_source::bundled_extension, "fs-extra" ) ) );
	CHECK( registry.remove_owner( "fs-extra" ) == 1 );
	REQUIRE( registry.add( make( "todo", tool_source::bundled_extension, "fs-extra" ) ) );
	CHECK( registry.size( ) == 1 );
}

TEST_CASE( "clear empties the registry", "[registry]" ) {
	auto registry = tool_registry{ };
	REQUIRE( registry.add( make( "read", tool_source::core ) ) );
	CHECK_FALSE( registry.empty( ) );

	registry.clear( );

	CHECK( registry.empty( ) );
	CHECK( registry.size( ) == 0 );
}

TEST_CASE( "class and source render as stable strings", "[registry]" ) {
	CHECK( mcode::to_string( tool_class::read ) == "read" );
	CHECK( mcode::to_string( tool_class::exec ) == "exec" );
	CHECK( mcode::to_string( tool_class::spawn ) == "spawn" );
	CHECK( mcode::to_string( tool_source::core ) == "core" );
	CHECK( mcode::to_string( tool_source::bundled_extension ) == "bundled" );
}
