#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include "mcode/core/registry.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/tools/errors.hxx"
#include "mcode/perm/argv.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/tools/exec_tools.hxx"
#include "mcode/tools/file_tools.hxx"
#include "mcode/tools/register.hxx"
#include "mcode/tools/schemas.hxx"
#include "mcode/tools/search_tools.hxx"
#include "mcode/tools/session_reads.hxx"
#include "mcode/tools/tool_args.hxx"
#include "mcode/tools/truncate.hxx"

#include "test_scratch.hxx"

using namespace mcode;
using namespace mcode::tools;

#include "tools_test_helpers.hxx"

using namespace tools_test;

TEST_CASE( "every core schema parses, validates and fits the budget", "[tools][schemas]" ) {
	auto registry = tool_registry{ };
	auto sink = stub_sink{ };
	auto setup = fixture{ };

	const auto registered = register_core_tools( registry, sink, setup.context );
	REQUIRE( registered );
	REQUIRE( registry.size( ) == 8 );
	REQUIRE( sink.names.size( ) == 8 );

	auto total_bytes = std::size_t{ 0 };

	for ( const auto* definition : registry.all( ) ) {
		REQUIRE( !definition->schema_json.empty( ) );
		REQUIRE( static_cast< bool >( validate_schema( definition->schema_json ) ) );

		const auto parsed = json::document::parse( definition->schema_json );
		REQUIRE( parsed );
		REQUIRE( parsed->get_string( "type" ).value_or( "" ) == "object" );

		total_bytes += definition->schema_json.size( );
	}

	INFO( "schema bytes: " << total_bytes );
	CHECK( total_bytes / TOKEN_CHARS_PER_TOKEN <= CORE_SCHEMA_BUDGET_TOKENS );
}

TEST_CASE( "a malformed schema is refused at registration", "[tools][schemas]" ) {
	CHECK_FALSE( static_cast< bool >( validate_schema( "{not json" ) ) );
	CHECK_FALSE( static_cast< bool >( validate_schema( R"({"type":"array"})" ) ) );
	CHECK_FALSE( static_cast< bool >(
		validate_schema( R"({"type":"object","required":["missing"]})" ) ) );
}

TEST_CASE( "read returns numbered lines and records the hash", "[tools][read]" ) {
	auto setup = fixture{ };

	const auto out = run_tool( handle_read, R"({"path":"notes.txt"})", setup );
	CHECK( out.find( "1\talpha" ) != std::string::npos );
	CHECK( out.find( "2\tbeta" ) != std::string::npos );

	const auto absolute = setup.space.resolve( "notes.txt" );
	REQUIRE( absolute );
	CHECK( setup.reads.contains( *absolute ) );
}

TEST_CASE( "read honours offset and limit and reports the next offset", "[tools][read]" ) {
	auto setup = fixture{ };

	const auto out = run_tool( handle_read, R"({"path":"notes.txt","offset":2,"limit":1})",
		setup );
	CHECK( out.find( "2\tbeta" ) != std::string::npos );
	CHECK( out.find( "1\talpha" ) == std::string::npos );
	CHECK( out.find( "\"next_offset\":3" ) != std::string::npos );
}

TEST_CASE( "read rejects out-of-range offsets and clamps the maximum window", "[tools][read]" ) {
	auto setup = fixture{ };

	const auto cases = std::vector< std::pair< const char*, const char* > >{
		{ R"({"path":"notes.txt","offset":0})", "offset must be 1-based" },
		{ R"({"path":"notes.txt","offset":-3})", "offset must be 1-based" },
		{ R"({"path":"notes.txt","offset":99})", "past the end" },
		{ R"({"path":"notes.txt","limit":0})", "limit must be positive" },
		{ R"({"path":"notes.txt","limit":-1})", "limit must be positive" },
	};

	for ( const auto& [ request, message ] : cases ) {
		const auto out = run_tool( handle_read, request, setup );
		CHECK( is_error_json( out ) );
		CHECK( out.find( message ) != std::string::npos );
	}

	auto many = std::string{ };

	for ( auto index = 1; index <= 1200; ++index ) {
		many += "line " + std::to_string( index ) + "\n";
	}

	setup.write_raw( "many.txt", many );

	const auto clamped = run_tool( handle_read, R"({"path":"many.txt","limit":5000})", setup );
	auto tabs = std::size_t{ 0 };

	for ( const auto character : clamped ) {
		if ( character == '\t' ) {
			++tabs;
		}
	}

	CHECK( tabs == 1000 );
	CHECK( clamped.find( "\"next_offset\":1001" ) != std::string::npos );
}

TEST_CASE( "read serves a partial window from a file above the window bound", "[tools][read]" ) {
	auto setup = fixture{ };
	const auto huge = std::string( static_cast< std::size_t >( 16u * 1024u * 1024u ) + 1, 'x' );
	setup.write_raw( "huge.txt", huge );

	const auto out = run_tool( handle_read, R"({"path":"huge.txt","limit":1})", setup );
	CHECK_FALSE( is_error_json( out ) );
	CHECK( out.find( "\"partial_read\":true" ) != std::string::npos );
}

TEST_CASE( "read refuses binary with a stub, never emitting bytes", "[tools][read]" ) {
	auto setup = fixture{ };
	setup.write_raw( "blob.bin", std::string{ 'a', '\0', 'b', '\0', 'c' } );

	const auto out = run_tool( handle_read, R"({"path":"blob.bin"})", setup );
	CHECK( is_error_json( out ) );
	CHECK( out.find( "binary" ) != std::string::npos );
	CHECK( out.find( "xxd" ) != std::string::npos );
}

TEST_CASE( "read of an empty file succeeds and records the read", "[tools][read]" ) {
	// zero lines is a legitimate state, not an offset past the end
	auto setup = fixture{ };
	setup.write_raw( "empty.txt", "" );

	const auto out = run_tool( handle_read, R"({"path":"empty.txt"})", setup );
	CHECK_FALSE( is_error_json( out ) );

	// an empty render must be distinguishable from a tool that produced nothing
	CHECK( out.find( "\"empty\":true" ) != std::string::npos );

	const auto absolute = setup.space.resolve( "empty.txt" );
	REQUIRE( absolute );
	CHECK( setup.reads.contains( *absolute ) );
}

TEST_CASE( "read on a missing file suggests the closest existing path", "[tools][read]" ) {
	auto setup = fixture{ };

	const auto out = run_tool( handle_read, R"({"path":"src/main.cx"})", setup );
	CHECK( is_error_json( out ) );
	CHECK( out.find( "src/main.cxx" ) != std::string::npos );
}

TEST_CASE( "write creates a new file without a prior read", "[tools][write]" ) {
	auto setup = fixture{ };

	const auto out = run_tool( handle_write,
		R"({"path":"src/new.cxx","content":"int x;\n"})", setup );
	CHECK( out.find( "\"ok\":true" ) != std::string::npos );
	CHECK( out.find( "\"mode\":\"create\"" ) != std::string::npos );
	CHECK( setup.read_raw( "src/new.cxx" ) == "int x;\n" );
}

TEST_CASE( "write refuses an overwrite without a prior read", "[tools][write]" ) {
	auto setup = fixture{ };

	const auto out = run_tool( handle_write,
		R"({"path":"notes.txt","content":"replaced\n"})", setup );
	CHECK( is_error_json( out ) );
	CHECK( out.find( "without reading it first" ) != std::string::npos );
	CHECK( setup.read_raw( "notes.txt" ).find( "alpha" ) != std::string::npos );
}

TEST_CASE( "write succeeds after a read and refuses a stale file", "[tools][write]" ) {
	auto setup = fixture{ };

	CHECK( run_tool( handle_read, R"({"path":"notes.txt"})", setup ).find( "alpha" ) !=
		std::string::npos );

	const auto replaced = run_tool( handle_write,
		R"({"path":"notes.txt","content":"replaced\n"})", setup );
	CHECK( replaced.find( "\"ok\":true" ) != std::string::npos );
	CHECK( replaced.find( "\"mode\":\"overwrite\"" ) != std::string::npos );

	// a write after an external change to the recorded read is stale
	setup.write_raw( "notes.txt", "externally changed\n" );

	const auto stale = run_tool( handle_write,
		R"({"path":"notes.txt","content":"second replace\n"})", setup );
	CHECK( is_error_json( stale ) );
	CHECK( stale.find( "changed since it was last read" ) != std::string::npos );
}

TEST_CASE( "write refuses protected and escaping paths", "[tools][write]" ) {
	auto setup = fixture{ };

	for ( const auto* target : { ".mcode/config.toml", ".mcode/artifacts/x", ".git/hooks/pre-commit" } ) {
		const auto out = run_tool( handle_write,
			std::string{ R"({"path":")" } + target + R"(","content":"x"})", setup );
		CHECK( is_error_json( out ) );
		CHECK( out.find( "refusing to write" ) != std::string::npos );
	}

	const auto outside = run_tool( handle_write,
		R"({"path":"../outside.txt","content":"x"})", setup );
	CHECK( is_error_json( outside ) );
}

TEST_CASE( "write enforces the size cap outside the workspace", "[tools][write]" ) {
	auto setup = fixture{ true };
	const auto oversized = std::string(
		static_cast< std::size_t >( MAX_WRITE_FILE_BYTES ) + 1, 'x' );

	const auto out = run_tool( handle_write,
		std::string{ R"({"path":"../oversized.txt","content":")" } + oversized + R"("})",
		setup );
	CHECK( is_error_json( out ) );
	CHECK( out.find( "write cap" ) != std::string::npos );
	CHECK_FALSE( std::filesystem::exists( setup.path.parent_path( ) / "oversized.txt" ) );
}

TEST_CASE( "edit replaces a unique anchor and returns a diff", "[tools][edit]" ) {
	auto setup = fixture{ };

	CHECK( run_tool( handle_read, R"({"path":"src/main.cxx"})", setup ).find( "int main" ) !=
		std::string::npos );

	const auto out = run_tool( handle_edit,
		R"({"path":"src/main.cxx","old_string":"return 0;","new_string":"return 42;"})",
		setup );
	CHECK( out.find( "\"ok\":true" ) != std::string::npos );
	CHECK( out.find( "\"replacements\":1" ) != std::string::npos );
	CHECK( out.find( "--- a/src/main.cxx" ) != std::string::npos );
	CHECK( out.find( "+\\treturn 42;\\n" ) != std::string::npos );
	CHECK( setup.read_raw( "src/main.cxx" ).find( "return 42;" ) != std::string::npos );
}

TEST_CASE( "edit distinguishes zero from multiple matches", "[tools][edit]" ) {
	auto setup = fixture{ };
	setup.write_raw( "dup.txt", "same\nsame\n" );
	CHECK( run_tool( handle_read, R"({"path":"dup.txt"})", setup ).find( "same" ) !=
		std::string::npos );

	const auto zero = run_tool( handle_edit,
		R"({"path":"dup.txt","old_string":"absent","new_string":"x"})", setup );
	CHECK( is_error_json( zero ) );
	CHECK( zero.find( "not found" ) != std::string::npos );

	const auto multiple = run_tool( handle_edit,
		R"({"path":"dup.txt","old_string":"same","new_string":"x"})", setup );
	CHECK( is_error_json( multiple ) );
	CHECK( multiple.find( "appears 2 times" ) != std::string::npos );

	const auto all = run_tool( handle_edit,
		R"({"path":"dup.txt","old_string":"same","new_string":"x","replace_all":true})", setup );
	CHECK( all.find( "\"replacements\":2" ) != std::string::npos );
}

TEST_CASE( "edit counts a self-overlapping anchor as disjoint matches", "[tools][edit]" ) {
	auto setup = fixture{ };
	setup.write_raw( "overlap.txt", "aaaa\n" );
	CHECK( run_tool( handle_read, R"({"path":"overlap.txt"})", setup ).find( "aaaa" ) !=
		std::string::npos );

	const auto guarded = run_tool( handle_edit,
		R"({"path":"overlap.txt","old_string":"aa","new_string":"b"})", setup );
	CHECK( is_error_json( guarded ) );
	CHECK( guarded.find( "appears 2 times" ) != std::string::npos );

	const auto all = run_tool( handle_edit,
		R"({"path":"overlap.txt","old_string":"aa","new_string":"b","replace_all":true})", setup );
	CHECK( all.find( "\"replacements\":2" ) != std::string::npos );
	CHECK( setup.read_raw( "overlap.txt" ) == "bb\n" );
}

TEST_CASE( "edit preserves CRLF and refuses an empty anchor", "[tools][edit]" ) {
	auto setup = fixture{ };
	setup.write_raw( "crlf.txt", "first\r\nsecond\r\n" );
	CHECK( run_tool( handle_read, R"({"path":"crlf.txt"})", setup ).find( "first" ) !=
		std::string::npos );

	const auto out = run_tool( handle_edit,
		R"({"path":"crlf.txt","old_string":"second","new_string":"changed"})", setup );
	CHECK( out.find( "\"ok\":true" ) != std::string::npos );

	auto raw = std::string{ };
	auto in = std::ifstream{ setup.path / "crlf.txt", std::ios::binary };
	std::getline( in, raw );
	REQUIRE( raw.size( ) > 5 );
	CHECK( raw.substr( raw.size( ) - 1 ) == "\r" );

	auto second = std::string{ };
	std::getline( in, second );
	CHECK( second == "changed\r" );

	const auto empty = run_tool( handle_edit,
		R"({"path":"crlf.txt","old_string":"","new_string":"x"})", setup );
	CHECK( is_error_json( empty ) );
	CHECK( empty.find( "use write" ) != std::string::npos );
}

TEST_CASE( "edit requires a prior read", "[tools][edit]" ) {
	auto setup = fixture{ };

	const auto out = run_tool( handle_edit,
		R"({"path":"src/main.cxx","old_string":"return 0;","new_string":"return 1;"})",
		setup );
	CHECK( is_error_json( out ) );
	CHECK( out.find( "without reading it first" ) != std::string::npos );
}

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

TEST_CASE( "an over-cap result spills to an artifact with an actionable notice",
	"[tools][truncate]" ) {
	auto setup = fixture{ };

	auto big = std::string{ };

	for ( auto index = 0; index < 1000; ++index ) {
		big += "line of output that keeps going\n";
	}

	const auto out = truncate_result( big, setup.context.run_id, setup.space, "test" );
	CHECK( out.size( ) <= INLINE_RESULT_CHARS + 512 );
	CHECK( out.find( ".mcode/artifacts/test-run/test-" ) != std::string::npos );
	CHECK( out.find( "Full output written to" ) != std::string::npos );

	const auto path_start = out.find( ".mcode/artifacts/" );
	const auto path_end = out.find( ';', path_start );
	REQUIRE( path_end != std::string::npos );
	const auto spill_path = out.substr( path_start, path_end - path_start );

	const auto spilled = setup.space.read_file( spill_path );
	REQUIRE( spilled );
	CHECK( *spilled == big );
}

TEST_CASE( "every tool failure carries error, hint and retryable", "[tools][errors]" ) {
	auto setup = fixture{ };

	const auto samples = std::vector< std::string >{
		run_tool( handle_read, R"({"path":"missing.txt"})", setup ),
		run_tool( handle_write, R"({"path":"notes.txt","content":"x"})", setup ),
		run_tool( handle_edit, R"({"path":"notes.txt","old_string":"a","new_string":"b"})", setup ),
		run_tool( handle_bash, R"({"command":"echo a && echo b"})", setup ),
		run_tool( handle_ask_user, R"({"question":"q"})", setup ),
	};

	for ( const auto& sample : samples ) {
		CHECK( is_error_json( sample ) );
	}
}

TEST_CASE( "a second edit in the same turn succeeds", "[tools][edit]" ) {
	auto setup = fixture{ };

	CHECK( run_tool( handle_read, R"({"path":"src/main.cxx"})", setup ).find( "int main" ) !=
		std::string::npos );

	const auto first = run_tool( handle_edit,
		R"({"path":"src/main.cxx","old_string":"return 0;","new_string":"return 1;"})",
		setup );
	CHECK( first.find( "\"ok\":true" ) != std::string::npos );

	const auto second = run_tool( handle_edit,
		R"({"path":"src/main.cxx","old_string":"return 1;","new_string":"return 2;"})",
		setup );
	CHECK( second.find( "\"ok\":true" ) != std::string::npos );
	CHECK( setup.read_raw( "src/main.cxx" ).find( "return 2;" ) != std::string::npos );
}

TEST_CASE( "edit refuses a file changed on disk after the read", "[tools][edit]" ) {
	auto setup = fixture{ };

	CHECK( run_tool( handle_read, R"({"path":"src/main.cxx"})", setup ).find( "int main" ) !=
		std::string::npos );

	setup.write_raw( "src/main.cxx", "int main( ) {\n\treturn 7;\n}\n" );

	const auto out = run_tool( handle_edit,
		R"({"path":"src/main.cxx","old_string":"return 0;","new_string":"return 1;"})",
		setup );
	CHECK( is_error_json( out ) );
	CHECK( out.find( "changed since it was last read" ) != std::string::npos );
}

TEST_CASE( "read of a non-UTF-8 file does not make the first write stale", "[tools][read]" ) {
	auto setup = fixture{ };
	setup.write_raw( "latin.txt", std::string{ 'a', '\xE9', 'b', '\n' } );

	CHECK( run_tool( handle_read, R"({"path":"latin.txt"})", setup ).find( "a" ) !=
		std::string::npos );

	const auto out = run_tool( handle_write,
		R"({"path":"latin.txt","content":"replaced\n"})", setup );
	CHECK( out.find( "\"ok\":true" ) != std::string::npos );
	CHECK( out.find( "\"mode\":\"overwrite\"" ) != std::string::npos );
}

TEST_CASE( "deny beats allow regardless of scope order", "[perm][engine]" ) {
	auto setup = fixture{ };

	setup.engine.add_config_rules( perm::rule_scope::user, { }, { "git status" } );
	setup.engine.add_config_rules( perm::rule_scope::session, { "git status" }, { } );

	auto request = perm::permission_request{ };
	request.tool_name = "bash";
	request.klass = tool_class::exec;
	request.resource = "git status";

	CHECK( setup.engine.decide( request ) == perm::permission_decision::deny );

	auto swapped = fixture{ };
	swapped.engine.add_config_rules( perm::rule_scope::user, { "git status" }, { } );
	swapped.engine.add_config_rules( perm::rule_scope::session, { }, { "git status" } );

	CHECK( swapped.engine.decide( request ) == perm::permission_decision::deny );
}

TEST_CASE( "a remembered allow is argv-exact", "[perm][engine]" ) {
	auto setup = fixture{ };

	setup.approval.queue( perm::approval_outcome::allow_remember );

	auto status = perm::permission_request{ };
	status.tool_name = "bash";
	status.klass = tool_class::exec;
	status.resource = "git status";

	CHECK( setup.engine.decide( status ) == perm::permission_decision::allow );

	auto push = perm::permission_request{ };
	push.tool_name = "bash";
	push.klass = tool_class::exec;
	push.resource = "git push";

	setup.approval.queue( perm::approval_outcome::deny_once );
	CHECK( setup.engine.decide( push ) == perm::permission_decision::deny );
}

TEST_CASE( "redirection and newline are refused as command shape", "[perm][argv]" ) {
	CHECK_FALSE( perm::parse_command_line( "git log > .git/hooks/pre-commit" ).has_value( ) );
	CHECK_FALSE( perm::parse_command_line( "sort < input.txt" ).has_value( ) );
	CHECK_FALSE( perm::parse_command_line( std::string{ "echo a\nrm -rf /" } ).has_value( ) );
	CHECK_FALSE( perm::parse_command_line( std::string{ "echo a\rb" } ).has_value( ) );
}

TEST_CASE( "runner detection survives spelling differences", "[perm][argv]" ) {
	CHECK( perm::is_exec_runner( "/bin/sh" ) );
	CHECK( perm::is_exec_runner( "C:\\Windows\\System32\\cmd.exe" ) );
	CHECK( perm::is_exec_runner( "CMD" ) );
	CHECK( perm::is_exec_runner( "Cmd.exe" ) );
	CHECK( perm::is_exec_runner( "pwsh" ) );
	CHECK( perm::is_exec_runner( "bash.exe" ) );
	CHECK_FALSE( perm::is_exec_runner( "git" ) );
	CHECK_FALSE( perm::is_exec_runner( "cmake" ) );
}

TEST_CASE( "variable expansion is refused", "[perm][argv]" ) {
	CHECK_FALSE( perm::parse_command_line( "echo $HOME" ).has_value( ) );
	CHECK_FALSE( perm::parse_command_line( "echo ${PATH}" ).has_value( ) );
	CHECK_FALSE( perm::parse_command_line( "echo %APPDATA%" ).has_value( ) );
}

TEST_CASE( "two truncated results in one run spill to distinct artifacts",
	"[tools][truncate]" ) {
	auto setup = fixture{ };

	auto big = std::string{ };

	for ( auto index = 0; index < 1000; ++index ) {
		big += "line of output that keeps going\n";
	}

	const auto first = truncate_result( big, setup.context.run_id, setup.space, "bash" );
	const auto second = truncate_result( big, setup.context.run_id, setup.space, "bash" );

	CHECK( first.find( "truncation failed" ) == std::string::npos );
	CHECK( second.find( "truncation failed" ) == std::string::npos );

	const auto first_path = first.substr( first.find( ".mcode/artifacts/" ) );
	const auto second_path = second.substr( second.find( ".mcode/artifacts/" ) );
	CHECK( first_path != second_path );
}

TEST_CASE( "a fenced or prose-wrapped argument payload is repaired", "[tools][repair]" ) {
	auto fenced = prepare_arguments( READ_SCHEMA, "```json\n{\"path\":\"src/main.cxx\"}\n```" );
	REQUIRE( fenced.ok( ) );
	CHECK( fenced.json == R"({"path":"src/main.cxx"})" );

	auto prose = prepare_arguments( READ_SCHEMA,
		"Sure, here are the arguments:\n{\"path\":\"notes.txt\"}\nLet me know if you need more." );
	REQUIRE( prose.ok( ) );
	CHECK( prose.json == R"({"path":"notes.txt"})" );

	auto trailing = prepare_arguments( READ_SCHEMA, "{\"path\":\"notes.txt\",}" );
	REQUIRE( trailing.ok( ) );
	CHECK( trailing.json == R"({"path":"notes.txt"})" );

	auto python = prepare_arguments( READ_SCHEMA, "{'path': 'notes.txt', 'limit': None}" );
	REQUIRE( python.ok( ) );
	CHECK( python.json.find( "notes.txt" ) != std::string::npos );
}

TEST_CASE( "a clean payload passes through byte-identical", "[tools][repair]" ) {
	auto prepared = prepare_arguments( READ_SCHEMA, R"({"path":"src/main.cxx","limit":50})" );

	REQUIRE( prepared.ok( ) );
	CHECK( prepared.json == R"({"path":"src/main.cxx","limit":50})" );
}

TEST_CASE( "repair never invents content", "[tools][repair]" ) {
	// a payload cut off inside a string cannot be completed without inventing the value
	auto inside_string = prepare_arguments( READ_SCHEMA, R"({"path":"src/ma)" );

	CHECK_FALSE( inside_string.ok( ) );

	// one cut off after a colon is missing the value entirely, so it is unrecoverable too
	auto after_colon = prepare_arguments( READ_SCHEMA, R"({"path":"src/main.cxx","limit":)" );

	CHECK_FALSE( after_colon.ok( ) );

	// a payload cut off at a member boundary loses only the incomplete member
	auto boundary = prepare_arguments( READ_SCHEMA, R"({"path":"src/main.cxx","limit":5)" );

	REQUIRE( boundary.ok( ) );
	CHECK( boundary.json == R"({"path":"src/main.cxx","limit":5})" );
}

TEST_CASE( "a payload with nothing to recover is still an error", "[tools][repair]" ) {
	CHECK_FALSE( prepare_arguments( READ_SCHEMA, "not json at all" ).ok( ) );
	CHECK_FALSE( prepare_arguments( READ_SCHEMA, "" ).ok( ) );
}

TEST_CASE( "arguments are checked against the declared schema", "[tools][schema]" ) {
	// a required member that is absent names the parameter and the admissible set
	auto missing = prepare_arguments( READ_SCHEMA, R"({"limit":5})" );
	CHECK_FALSE( missing.ok( ) );
	CHECK( missing.failure.find( "path" ) != std::string::npos );
	CHECK( missing.failure.find( "is required but missing" ) != std::string::npos );

	// a wrong primitive type names both the observed and the expected type
	auto wrong_type = prepare_arguments( READ_SCHEMA, R"({"path":5})" );
	CHECK_FALSE( wrong_type.ok( ) );
	CHECK( wrong_type.failure.find( "path" ) != std::string::npos );
	CHECK( wrong_type.failure.find( "expected string" ) != std::string::npos );

	// a well-formed payload passes and is returned unchanged
	auto good = prepare_arguments( READ_SCHEMA, R"({"path":"src/main.cxx"})" );
	REQUIRE( good.ok( ) );
	CHECK( good.json == R"({"path":"src/main.cxx"})" );
}

TEST_CASE( "an enum parameter reports the values it admits", "[tools][schema]" ) {
	const auto schema = std::string{ R"JSON({
		"type": "object",
		"properties": { "mode": { "type": "string", "enum": [ "fast", "slow" ] } },
		"required": [ "mode" ]
	})JSON" };

	auto bad = prepare_arguments( schema, R"({"mode":"medium"})" );
	CHECK_FALSE( bad.ok( ) );
	CHECK( bad.failure.find( "mode" ) != std::string::npos );
	CHECK( bad.failure.find( "fast" ) != std::string::npos );
	CHECK( bad.failure.find( "slow" ) != std::string::npos );

	auto ok = prepare_arguments( schema, R"({"mode":"fast"})" );
	CHECK( ok.ok( ) );
}

TEST_CASE( "an unconstrained tool accepts any object", "[tools][schema]" ) {
	// an empty schema is what an MCP server with no declared parameters registers
	auto prepared = prepare_arguments( "", R"({"anything":"goes"})" );

	REQUIRE( prepared.ok( ) );
	CHECK( prepared.json.find( "anything" ) != std::string::npos );
}

TEST_CASE( "a truncated call reports truncation rather than a malformed payload",
	"[tools][errors]" ) {
	const auto message = truncation_error( "read" );

	CHECK( is_error_json( message ) );
	CHECK( message.find( "read" ) != std::string::npos );
	CHECK( message.find( "cut off" ) != std::string::npos );
	// truncation is retryable: a smaller response can succeed where this one did not
	CHECK( message.find( "\"retryable\":true" ) != std::string::npos );
}

TEST_CASE( "a nested parameter schema is refused at registration", "[tools][schema]" ) {
	const auto nested = std::string{ R"JSON({
		"type": "object",
		"properties": { "options": { "type": "object" } }
	})JSON" };

	const auto nested_status = validate_schema( nested );
	CHECK_FALSE( static_cast< bool >( nested_status ) );

	const auto array_of_objects = std::string{ R"JSON({
		"type": "object",
		"properties": { "items": { "type": "array", "items": { "type": "object" } } }
	})JSON" };

	CHECK_FALSE( static_cast< bool >( validate_schema( array_of_objects ) ) );

	const auto flat = std::string{ R"JSON({
		"type": "object",
		"properties": { "path": { "type": "string" }, "tags": { "type": "array", "items": { "type": "string" } } },
		"required": [ "path" ]
	})JSON" };

	CHECK( static_cast< bool >( validate_schema( flat ) ) );
}
