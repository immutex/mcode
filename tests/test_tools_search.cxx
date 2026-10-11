#include <catch2/catch_test_macros.hpp>

#include <string>

#include "test_scratch.hxx"

#include "tools_test_helpers.hxx"

using namespace mcode;
using namespace mcode::tools;
using namespace tools_test;

TEST_CASE( "glob returns relative paths and skips .git and .mcode", "[tools][glob]" ) {
	auto setup = fixture{ };
	setup.write_raw( ".git/config", "x" );
	setup.write_raw( ".mcode/artifacts/run/out.txt", "x" );
	setup.write_raw( "src/deep/nested.cxx", "x" );

	const auto out = run_tool( handle_glob, R"({"pattern":"**/*"})", setup );
	CHECK( out.find( "\"src/main.cxx\"" ) != std::string::npos );
	CHECK( out.find( "\"src/deep/nested.cxx\"" ) != std::string::npos );
	CHECK( out.find( ".git" ) == std::string::npos );
	CHECK( out.find( ".mcode" ) == std::string::npos );
}

TEST_CASE( "glob honours the root .gitignore", "[tools][glob]" ) {
	auto setup = fixture{ };
	setup.write_raw( ".gitignore", "ignored-dir/\n*.tmp\n" );
	setup.write_raw( "ignored-dir/thing.o", "x" );
	setup.write_raw( "scratch.tmp", "x" );
	setup.write_raw( "kept.cxx", "x" );

	const auto out = run_tool( handle_glob, R"({"pattern":"**/*"})", setup );
	CHECK( out.find( "ignored-dir" ) == std::string::npos );
	CHECK( out.find( "scratch.tmp" ) == std::string::npos );
	CHECK( out.find( "kept.cxx" ) != std::string::npos );
}

TEST_CASE( "grep caps matches during the scan and skips binary", "[tools][grep]" ) {
	auto setup = fixture{ };

	auto repeated = std::string{ };

	for ( auto index = 0; index < 80; ++index ) {
		repeated += "needle here\n";
	}

	setup.write_raw( "many.txt", repeated );
	setup.write_raw( "blob.bin", std::string{ 'n', '\0', 'e', '\0' } );

	const auto out = run_tool( handle_grep, R"({"pattern":"needle"})", setup );
	CHECK( out.find( "\"count\":50" ) != std::string::npos );
	CHECK( out.find( "cap of 50" ) != std::string::npos );
	CHECK( out.find( "blob.bin" ) == std::string::npos );
}

TEST_CASE( "grep reports a bad regex actionably", "[tools][grep]" ) {
	auto setup = fixture{ };

	const auto out = run_tool( handle_grep, R"({"pattern":"([unclosed"})", setup );
	CHECK( is_error_json( out ) );
	CHECK( out.find( "invalid regex" ) != std::string::npos );
}

TEST_CASE( "grep stops at the final line and fails closed on a bad path", "[tools][grep]" ) {
	auto setup = fixture{ };

	const auto anchors = run_tool( handle_grep, R"({"pattern":"^","path":"notes.txt"})", setup );
	CHECK( anchors.find( "\"count\":3" ) != std::string::npos );
	CHECK( anchors.find( "\"line\":4" ) == std::string::npos );

	const auto missing = run_tool( handle_grep, R"({"pattern":"alpha","path":"nope.txt"})", setup );
	CHECK( is_error_json( missing ) );
	CHECK( missing.find( "not found" ) != std::string::npos );

	const auto outside = run_tool( handle_grep, R"({"pattern":"alpha","path":"../escape"})", setup );
	CHECK( is_error_json( outside ) );
}

TEST_CASE( "ask_user fails clearly when headless", "[tools][ask_user]" ) {
	auto setup = fixture{ };

	const auto out = run_tool( handle_ask_user,
		R"({"question":"which design?"})", setup );
	CHECK( is_error_json( out ) );
	CHECK( out.find( "which design?" ) != std::string::npos );
}

TEST_CASE( "tool_search returns names then expands without mutating the registry",
	"[tools][tool_search]" ) {
	auto setup = fixture{ };
	auto registry = tool_registry{ };
	auto sink = stub_sink{ };

	REQUIRE( static_cast< bool >( register_core_tools( registry, sink, setup.context ) ) );
	const auto before = registry.size( );

	const auto listed = run_tool(
		[ & ]( const tool_args& args, tool_context& context ) {
			return handle_tool_search( args, context, registry );
		}, R"({"query":"read"})", setup );
	CHECK( listed.find( "\"name\":\"read\"" ) != std::string::npos );
	CHECK( listed.find( "\"schema\"" ) == std::string::npos );

	const auto expanded = run_tool(
		[ & ]( const tool_args& args, tool_context& context ) {
			return handle_tool_search( args, context, registry );
		}, R"({"query":"read","expand":true})", setup );
	CHECK( expanded.find( "\"schema\"" ) != std::string::npos );
	CHECK( registry.size( ) == before );
}
