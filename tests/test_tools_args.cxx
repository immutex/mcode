#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/tools/schemas.hxx"

#include "test_scratch.hxx"

#include "tools_test_helpers.hxx"

using namespace mcode;
using namespace mcode::tools;
using namespace tools_test;

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
	auto missing = prepare_arguments( READ_SCHEMA, R"({"limit":5})" );
	CHECK_FALSE( missing.ok( ) );
	CHECK( missing.failure.find( "path" ) != std::string::npos );
	CHECK( missing.failure.find( "is required but missing" ) != std::string::npos );

	auto wrong_type = prepare_arguments( READ_SCHEMA, R"({"path":5})" );
	CHECK_FALSE( wrong_type.ok( ) );
	CHECK( wrong_type.failure.find( "path" ) != std::string::npos );
	CHECK( wrong_type.failure.find( "expected string" ) != std::string::npos );

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
