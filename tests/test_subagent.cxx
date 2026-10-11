#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "mcode/agent/subagent.hxx"
#include "mcode/core/registry.hxx"

#include "test_scratch.hxx"

using namespace mcode;

namespace {

	auto definition( const std::string& name, const tool_class klass ) {
		auto value = tool_def{ };
		value.name = name;
		value.description = "test tool";
		value.klass = klass;
		value.schema_json = R"({"type":"object"})";

		return value;
	}

}

TEST_CASE( "the read-only registry drops every mutating tool", "[subagent]" ) {
	auto parent = tool_registry{ };

	REQUIRE( parent.add( definition( "read", tool_class::read ) ).has_value( ) );
	REQUIRE( parent.add( definition( "glob", tool_class::read ) ).has_value( ) );
	REQUIRE( parent.add( definition( "grep", tool_class::read ) ).has_value( ) );
	REQUIRE( parent.add( definition( "write", tool_class::write ) ).has_value( ) );
	REQUIRE( parent.add( definition( "edit", tool_class::write ) ).has_value( ) );
	REQUIRE( parent.add( definition( "bash", tool_class::exec ) ).has_value( ) );

	const auto child = read_only_registry( parent );

	REQUIRE( child.has_value( ) );
	REQUIRE( child->size( ) == 3 );
	REQUIRE( child->find( "read" ) != nullptr );
	REQUIRE( child->find( "glob" ) != nullptr );
	REQUIRE( child->find( "grep" ) != nullptr );

	// Absent from the child's registry entirely, not merely refused at dispatch.
	REQUIRE( child->find( "write" ) == nullptr );
	REQUIRE( child->find( "edit" ) == nullptr );
	REQUIRE( child->find( "bash" ) == nullptr );
}

TEST_CASE( "no shell reaches a read-only subagent", "[subagent]" ) {
	// Cline's Plan mode refuses file-editing tools but still runs a shell, and
	// `python -c` writes files.
	for ( const auto name : subagent_tool_names( ) ) {
		REQUIRE( name != "bash" );
		REQUIRE( name != "exec" );
	}
}

TEST_CASE( "the subagent prompt states the boundary", "[subagent]" ) {
	const auto prompt = subagent_system_prompt( );

	REQUIRE( prompt.find( "read-only" ) != std::string::npos );
	REQUIRE( prompt.find( "2000 tokens" ) != std::string::npos );
}

TEST_CASE( "a subagent with no parent is refused rather than run", "[subagent]" ) {
	auto request = subagent_request{ };
	request.objective = "look around";

	const auto result = run_subagent( request );

	REQUIRE( !result.has_value( ) );
	REQUIRE( result.error( ).msg.find( "parent" ) != std::string::npos );
}
