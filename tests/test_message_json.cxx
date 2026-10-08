#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/agent/message_json.hxx"

using namespace mcode::agent;

namespace {

	[[nodiscard]] auto text_block( const std::string& text ) -> mcode::model::block {
		auto block = mcode::model::block{ };
		block.kind = mcode::model::block_kind::text;
		block.text = text;

		return block;
	}

}

TEST_CASE( "a message survives a round trip through the log encoding", "[agent][message_json]" ) {
	SECTION( "a text message" ) {
		auto message = mcode::model::message{ };
		message.speaker = mcode::model::role::assistant;
		message.blocks.push_back( text_block( "ALPHA" ) );

		const auto encoded = message_to_json( message );
		auto decoded = message_from_json( encoded );

		REQUIRE( static_cast< bool >( decoded ) );
		CHECK( decoded->speaker == mcode::model::role::assistant );
		REQUIRE( decoded->blocks.size( ) == 1 );
		CHECK( decoded->blocks.front( ).kind == mcode::model::block_kind::text );

		// The regression: a dotted JSON path never matches, so the text came back
		// empty and a resumed session silently lost its transcript.
		CHECK( decoded->blocks.front( ).text == "ALPHA" );
	}

	SECTION( "text needing escaping" ) {
		auto message = mcode::model::message{ };
		message.speaker = mcode::model::role::user;
		message.blocks.push_back( text_block( "quote \" backslash \\ newline \n tab \t" ) );

		auto decoded = message_from_json( message_to_json( message ) );

		REQUIRE( static_cast< bool >( decoded ) );
		REQUIRE( decoded->blocks.size( ) == 1 );
		CHECK( decoded->blocks.front( ).text
			== "quote \" backslash \\ newline \n tab \t" );
	}

	SECTION( "a tool call keeps its id, name and args" ) {
		auto message = mcode::model::message{ };
		message.speaker = mcode::model::role::assistant;

		auto call = mcode::model::block{ };
		call.kind = mcode::model::block_kind::tool_call;
		call.tool_call_id = "call_1";
		call.tool_name = "read";
		call.args_json = R"({"path":"a.txt"})";
		message.blocks.push_back( std::move( call ) );

		auto decoded = message_from_json( message_to_json( message ) );

		REQUIRE( static_cast< bool >( decoded ) );
		REQUIRE( decoded->blocks.size( ) == 1 );
		CHECK( decoded->blocks.front( ).tool_call_id == "call_1" );
		CHECK( decoded->blocks.front( ).tool_name == "read" );
		CHECK( decoded->blocks.front( ).args_json == R"({"path":"a.txt"})" );
	}

	SECTION( "a tool result keeps its error flag" ) {
		auto message = mcode::model::message{ };
		message.speaker = mcode::model::role::assistant;

		auto result = mcode::model::block{ };
		result.kind = mcode::model::block_kind::tool_result;
		result.tool_call_id = "call_9";
		result.result_json = "boom";
		result.is_error = true;
		message.blocks.push_back( std::move( result ) );

		auto decoded = message_from_json( message_to_json( message ) );

		REQUIRE( static_cast< bool >( decoded ) );
		REQUIRE( decoded->blocks.size( ) == 1 );
		CHECK( decoded->blocks.front( ).is_error );
		CHECK( decoded->blocks.front( ).result_json == "boom" );
	}
}

TEST_CASE( "a history survives a round trip with order intact",
	"[agent][message_json]" ) {
	auto history = std::vector< mcode::model::message >{ };

	auto user = mcode::model::message{ };
	user.speaker = mcode::model::role::user;
	user.blocks.push_back( text_block( "do the thing" ) );
	history.push_back( std::move( user ) );

	auto assistant = mcode::model::message{ };
	assistant.speaker = mcode::model::role::assistant;
	assistant.blocks.push_back( text_block( "done" ) );
	history.push_back( std::move( assistant ) );

	auto decoded = history_from_json( history_to_json( history ) );

	REQUIRE( static_cast< bool >( decoded ) );
	REQUIRE( decoded->size( ) == 2 );
	CHECK( decoded->at( 0 ).speaker == mcode::model::role::user );
	CHECK( decoded->at( 0 ).blocks.front( ).text == "do the thing" );
	CHECK( decoded->at( 1 ).speaker == mcode::model::role::assistant );
	CHECK( decoded->at( 1 ).blocks.front( ).text == "done" );
}

TEST_CASE( "malformed encodings are refused, not silently emptied",
	"[agent][message_json]" ) {
	CHECK_FALSE( static_cast< bool >( message_from_json( "not json" ) ) );
	CHECK_FALSE( static_cast< bool >( message_from_json( R"({"blocks":[]})" ) ) );
	CHECK_FALSE( static_cast< bool >(
		message_from_json( R"({"role":"nonsense","blocks":[]})" ) ) );
}
