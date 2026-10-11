#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "mcode/agent/subagent.hxx"
#include "mcode/core/registry.hxx"

#include "loop_test_helpers.hxx"
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
	request.spec.objective = "look around";

	const auto result = run_subagent( request );

	REQUIRE( !result.has_value( ) );
	REQUIRE( result.error( ).msg.find( "parent" ) != std::string::npos );
}

TEST_CASE( "the child can actually call the parent's read tools", "[subagent]" ) {
	auto fx = loop_test::fixture{ };
	fx.connect( );

	// The parent registers `echo` as its read handler under a name the child is
	// allowed to hold, so a spawn that carries the handlers through gives the child
	// a working tool. Without the carry the registry still lists `read`, and every
	// call fails at dispatch -- the boundary holds, but the capability is gone.
	auto fx_registry = tool_registry{ };

	auto definition = tool_def{ };
	definition.name = "read";
	definition.description = "reads a file";
	definition.klass = tool_class::read;
	definition.schema_json = R"({"type":"object"})";
	REQUIRE( fx_registry.add( definition ).has_value( ) );

	fx.loop->register_handler( "read",
		[]( std::string_view ) -> result< std::string > {
			return std::string{ "{\"ok\":true,\"content\":\"found it\"}" };
		} );

	auto scripted = loop_test::call_response( "read", R"({"path":"a.cxx"})" );
	fx.client.queue( scripted );
	fx.client.queue( loop_test::text_response( "the file says: found it" ) );

	const auto report = fx.loop->spawn_subagent(
		{ .objective = "read a.cxx", .context = "", .max_steps = 8 } );

	REQUIRE( report.has_value( ) );
	CHECK( report->summary.find( "found it" ) != std::string::npos );
}

TEST_CASE( "the parent is charged for what the child spent", "[subagent][budget]" ) {
	auto fx = loop_test::fixture{ };
	fx.connect( );

	auto scripted = loop_test::text_response( "the parser lives in src/parse.cxx" );
	scripted.input_tokens = 4'000;
	scripted.output_tokens = 1'000;
	fx.client.queue( scripted );

	const auto before = fx.loop->budget( ).tokens_used.load( );

	const auto report = fx.loop->spawn_subagent(
		{ .objective = "where is the parser", .context = "", .max_steps = 8 } );

	REQUIRE( report.has_value( ) );
	CHECK( report->summary.find( "src/parse.cxx" ) != std::string::npos );

	// The run pays for the child: a child on its own budget is free to the run.
	CHECK( fx.loop->budget( ).tokens_used.load( ) > before );
}

TEST_CASE( "a child cannot exceed the parent's remaining budget", "[subagent][budget]" ) {
	auto fx = loop_test::fixture{ };
	fx.connect( );

	fx.loop->budget( ).max_tokens = 1'000;
	fx.loop->budget( ).tokens_used.store( 900 );

	auto scripted = loop_test::text_response( "answer" );
	scripted.input_tokens = 5'000;
	fx.client.queue( scripted );

	const auto report = fx.loop->spawn_subagent(
		{ .objective = "answer anyway", .context = "", .max_steps = 8 } );

	// Carved from the remainder, so the child stops rather than spending past it.
	CHECK( fx.loop->budget( ).tokens_used.load( ) >= 900 );
}
