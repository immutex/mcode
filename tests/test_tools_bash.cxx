#include <catch2/catch_test_macros.hpp>

#include <string>

#include "mcode/agent/loop.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/perm/argv.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/tools/exec_tools.hxx"
#include "mcode/tools/tool_args.hxx"

#include "tools_test_helpers.hxx"

using namespace mcode;
using namespace mcode::tools;

using namespace tools_test;

// posix grep exits 2 on a missing file, so the fixture file must exist and be empty
#if defined( _WIN32 )
inline constexpr auto NO_MATCH_COMMAND = R"({"command":"findstr x missing.txt"})";
#else
inline constexpr auto NO_MATCH_COMMAND = R"({"command":"grep x /dev/null"})";
#endif

TEST_CASE( "bash prompts by default and honours a scripted allow", "[tools][bash]" ) {
	auto setup = fixture{ };

	// the loop, not the handler, is the single permission check point
	const auto tokens = perm::parse_command_line( "findstr x missing.txt" );
	REQUIRE( tokens.has_value( ) );

	auto request = perm::permission_request{ };
	request.tool_name = "bash";
	request.klass = tool_class::exec;
	request.resource = perm::canonical_argv( *tokens );

	const auto denied = setup.engine.decide( request );
	CHECK( denied == perm::permission_decision::deny );
	CHECK( setup.engine.last_verdict( ).matched.scope == "default" );
	CHECK( setup.approval.asks( ) == 1 );

	setup.approval.queue( perm::approval_outcome::allow_once );

	const auto allowed = run_tool( handle_bash, NO_MATCH_COMMAND, setup );
	CHECK( allowed.find( "\"ok\":true" ) != std::string::npos );
	CHECK( allowed.find( "\"exit_code\":1" ) != std::string::npos );
	CHECK( setup.approval.asks( ) == 1 );
}

TEST_CASE( "the loop denies bash and the handler never runs", "[perm][loop]" ) {
	auto setup = fixture{ };

	auto registry = tool_registry{ };
	auto shell = tool_def{ };
	shell.name = "bash";
	shell.klass = tool_class::exec;
	shell.schema_json = R"({"type":"object"})";
	REQUIRE( registry.add( shell ).has_value( ) );

	auto handler_runs = 0;
	auto loop_deps = agent_loop::dependencies{ };
	loop_deps.registry = &registry;
	loop_deps.permissions = &setup.engine;
	loop_deps.model_name = "test-model";
	auto loop = agent_loop{ loop_deps };

	loop.register_handler( "bash", [&]( std::string_view ) -> result< std::string > {
		++handler_runs;

		return std::string{ "{\"ok\":true}" };
	} );

	const auto outcome = loop.execute(
		tool_call{ "call-1", "bash", R"({"command":"findstr x missing.txt"})" } );

	CHECK_FALSE( outcome.ok );
	CHECK( outcome.permission_denied );
	CHECK( outcome.error_message.find( "denied by the permission engine" )
		!= std::string::npos );
	CHECK( handler_runs == 0 );
	CHECK( setup.approval.asks( ) == 1 );
}

TEST_CASE( "bash reports a non-zero exit as a success, never a tool error", "[tools][bash]" ) {
	auto setup = fixture{ true };

	const auto out = run_tool( handle_bash,
#if defined( _WIN32 )
		R"({"command":"findstr x missing-file.txt"})"
#else
		R"({"command":"false"})"
#endif
		, setup );
	CHECK( out.find( "\"ok\":true" ) != std::string::npos );
	CHECK( out.find( "\"exit_code\":1" ) != std::string::npos );
	CHECK( out.find( "never auto-retried" ) != std::string::npos );
}

TEST_CASE( "bash refuses compound commands and exec runners", "[tools][bash]" ) {
	auto setup = fixture{ true };

	const auto compound = run_tool( handle_bash,
		R"({"command":"echo a && echo b"})", setup );
	CHECK( is_error_json( compound ) );

	auto request = perm::permission_request{ };
	request.tool_name = "bash";
	request.klass = tool_class::exec;
	request.resource = "sh -c echo hi";

	CHECK( setup.engine.decide( request ) == perm::permission_decision::deny );
	CHECK( setup.engine.last_verdict( ).reason.find( "never allowlisted" )
		!= std::string::npos );
}

TEST_CASE( "bash executes the parsed argv, not the raw string", "[tools][bash]" ) {
	auto setup = fixture{ true };

	const auto out = run_tool( handle_bash, NO_MATCH_COMMAND, setup );
	CHECK( out.find( "\"ok\":true" ) != std::string::npos );
	CHECK( out.find( "\"exit_code\":1" ) != std::string::npos );
}
