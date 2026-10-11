#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "mcode/support/json.hxx"
#include "mcode/tools/schemas.hxx"

#include "test_scratch.hxx"

#include "tools_test_helpers.hxx"

using namespace mcode;
using namespace mcode::tools;
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
