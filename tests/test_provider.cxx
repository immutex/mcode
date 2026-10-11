#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "mcode/model/http_client.hxx"
#include "mcode/model/provider.hxx"

using namespace mcode;

namespace {

	const char* GATEWAY_DESCRIPTOR = R"({
		"name": "my-gateway",
		"endpoint": "https://api.example.com/v1/chat/completions",
		"auth": { "header": "Authorization", "from": "env", "name": "MY_GATEWAY_KEY" },
		"stream": {
			"text_delta": "/choices/0/delta/content",
			"tool_calls": {
				"index": "/choices/0/delta/tool_calls/0/index",
				"id": "/choices/0/delta/tool_calls/0/id",
				"name": "/choices/0/delta/tool_calls/0/function/name",
				"args": "/choices/0/delta/tool_calls/0/function/arguments"
			},
			"finish": "/choices/0/finish_reason",
			"usage": { "in": "/usage/prompt_tokens", "out": "/usage/completion_tokens" }
		}
	})";

	auto gateway( ) -> model::provider_descriptor {
		auto descriptor = model::descriptor_from_json( GATEWAY_DESCRIPTOR );

		if ( !descriptor ) {
			FAIL( "descriptor rejected: " << descriptor.error( ).msg );
		}

		return *descriptor;
	}

}

TEST_CASE( "the docs/26 descriptor parses and validates", "[provider]" ) {
	const auto descriptor = gateway( );

	REQUIRE( descriptor.name == "my-gateway" );
	REQUIRE( descriptor.endpoint == "https://api.example.com/v1/chat/completions" );
	REQUIRE( descriptor.auth.from == model::auth_spec::source::environment );
	REQUIRE( descriptor.auth.name == "MY_GATEWAY_KEY" );
	REQUIRE( descriptor.auth.header == "Authorization" );
	REQUIRE( descriptor.stream.text_delta == "/choices/0/delta/content" );
	REQUIRE( descriptor.stream.tool_call_args
		== "/choices/0/delta/tool_calls/0/function/arguments" );
	REQUIRE( descriptor.stream.usage_input == "/usage/prompt_tokens" );

	REQUIRE( static_cast< bool >( model::validate( descriptor ) ) );
}

TEST_CASE( "a descriptor that cannot work is rejected at load", "[provider]" ) {
	const auto cases = std::vector< std::pair< const char*, const char* > >{
		{ R"({"endpoint":"https://x/v1"})", "no name" },
		{ R"({"name":"x"})", "no endpoint" },
		{ R"({"name":"x","endpoint":"file:///etc/passwd","stream":{"text_delta":"/t"}})", "not http" },
		{ R"({"name":"x","endpoint":"api.example.com","stream":{"text_delta":"/t"}})", "bare host" },
		{ R"({"name":"x","endpoint":"https://x/v1"})", "maps nothing" },
		{ R"({"name":"x","endpoint":"https://x/v1","stream":{"text_delta":"choices.0.text"}})",
			"not a pointer" },
		{ R"({"name":"x","endpoint":"https://x/v1","stream":{"text_delta":"/t"},
			"auth":{"header":"Authorization"}})", "auth without a source" },
		{ R"({"name":"x","endpoint":"https://x/v1","stream":{"text_delta":"/t"},
			"auth":{"from":"env","header":"Authorization"}})", "auth without a name" },
		{ R"({"name":"x","endpoint":"https://x/v1","stream":{"text_delta":"/t"},
			"auth":{"from":"keyring","name":"k","header":"Authorization"}})", "unknown auth source" },
	};

	for ( const auto& [ json, label ] : cases ) {
		auto descriptor = model::descriptor_from_json( json );

		CHECK_FALSE( descriptor.has_value( ) );

		if ( descriptor ) {
			FAIL( "case '" << label << "' was accepted but should not be" );
		}
	}

	CHECK_FALSE( model::descriptor_from_json( "{not json" ).has_value( ) );
}

TEST_CASE( "an unknown descriptor key is refused at every level", "[provider]" ) {
	const auto cases = std::vector< std::pair< const char*, const char* > >{
		{ R"({"name":"x","endpoint":"https://x/v1","steam":{"text_delta":"/t"}})", "root typo" },
		{ R"({"name":"x","endpoint":"https://x/v1","on_event":true,"nope":1})", "root extra" },
		{ R"({"name":"x","endpoint":"https://x/v1","auth":{"from":"env","name":"K",
			"header":"Authorization","scheme2":"x"}})", "auth typo" },
		{ R"({"name":"x","endpoint":"https://x/v1","stream":{"text_delta":"/t",
			"tool_call":{"args":"/a"}}})", "stream typo" },
		{ R"({"name":"x","endpoint":"https://x/v1","stream":{"tool_calls":{"args":"/a",
			"names":"/n"}}})", "tool_calls typo" },
		{ R"({"name":"x","endpoint":"https://x/v1","stream":{"usage":{"in":"/i",
			"outt":"/o"}}})", "usage typo" },
		{ R"({"name":"x","endpoint":"https://x/v1","request":{"model":"m","nope":"x"}})",
			"request typo" },
	};

	for ( const auto& [ text, label ] : cases ) {
		auto descriptor = model::descriptor_from_json( text );

		CHECK_FALSE( static_cast< bool >( descriptor ) );

		if ( descriptor ) {
			FAIL( "case '" << label << "' loaded but should not have" );
		}

		if ( !descriptor ) {
			CHECK( descriptor.error( ).msg.find( "unknown descriptor key" ) != std::string::npos );
		}
	}

	auto accepted = model::descriptor_from_json( R"({
		"name": "full",
		"endpoint": "https://x/v1",
		"auth": { "from": "env", "name": "K", "header": "Authorization", "scheme": "Bearer" },
		"request": { "model": "model", "messages": "messages", "tools": "tools",
			"max_output_tokens": "max_tokens", "temperature": "temperature",
			"response_schema": "schema", "reasoning_effort": "effort" },
		"stream": {
			"text_delta": "/t",
			"thinking_delta": "/th",
			"tool_calls": { "index": "/i", "id": "/d", "name": "/n", "args": "/a" },
			"finish": "/f",
			"usage": { "in": "/ui", "out": "/uo", "cached_read": "/uc",
				"cache_write": "/uw", "reasoning": "/ur" },
			"terminal_events": ["done"],
			"text_events": ["text"],
			"tool_call_events": ["args"]
		},
		"extra_headers": "{}",
		"on_event": false
	})" );

	REQUIRE( static_cast< bool >( accepted ) );

	if ( !accepted ) {
		FAIL( accepted.error( ).msg );
	}

	REQUIRE( accepted->stream.usage_reasoning == "/ur" );
	REQUIRE( accepted->stream.text_events.size( ) == 1 );
	REQUIRE( accepted->stream.tool_call_events.size( ) == 1 );
}

TEST_CASE( "a pointer the applier cannot honour is refused at load", "[provider]" ) {
	// wildcards are unsupported in pointers.
	for ( const auto* pointer : { "/a/-", "/a/*/b", "*/b" } ) {
		auto text = std::string{ R"({"name":"x","endpoint":"https://x/v1","stream":{"text_delta":")" };
		text += pointer;
		text += R"("}})";

		auto descriptor = model::descriptor_from_json( text );

		CHECK_FALSE( static_cast< bool >( descriptor ) );

		if ( !descriptor ) {
			CHECK( descriptor.error( ).msg.find( "not a JSON pointer" ) != std::string::npos );
		}
	}
}

TEST_CASE( "a feature block parses and an unknown feature key is refused", "[provider]" ) {
	auto descriptor = model::descriptor_from_json( R"({
		"name": "x", "endpoint": "https://x/v1",
		"stream": { "text_delta": "/t" },
		"features": {
			"strict_tools": true,
			"parallel_tool_calls": true,
			"prefill_text": "<tool_call>",
			"response_format_with_tools": true
		}
	})" );

	REQUIRE( static_cast< bool >( descriptor ) );
	CHECK( descriptor->features.strict_tools );
	CHECK( descriptor->features.parallel_tool_calls );
	CHECK( descriptor->features.prefill_text == "<tool_call>" );
	CHECK( descriptor->features.response_format_with_tools );

	auto bare = model::descriptor_from_json( R"({
		"name": "x", "endpoint": "https://x/v1",
		"stream": { "text_delta": "/t" }
	})" );

	REQUIRE( static_cast< bool >( bare ) );
	CHECK_FALSE( bare->features.strict_tools );
	CHECK_FALSE( bare->features.parallel_tool_calls );
	CHECK_FALSE( bare->features.response_format_with_tools );
	CHECK( bare->features.prefill_text.empty( ) );

	CHECK_FALSE( static_cast< bool >( model::descriptor_from_json( R"({
		"name": "x", "endpoint": "https://x/v1",
		"stream": { "text_delta": "/t" },
		"features": { "not_a_feature": true }
	})" ) ) );
}

TEST_CASE( "a 400 naming a request field is classified for downgrade", "[provider]" ) {
	const auto strict = model::detect_feature_downgrade(
		R"({"error":{"message":"Unrecognized request argument supplied: strict"}})" );

	CHECK( strict.matched );
	CHECK( strict.field == "strict" );

	const auto parallel = model::detect_feature_downgrade(
		R"({"error":{"message":"parallel_tool_calls is not supported"}})" );

	CHECK( parallel.matched );
	CHECK( parallel.field == "parallel_tool_calls" );

	const auto prefill = model::detect_feature_downgrade(
		R"({"error":{"message":"final assistant content cannot end with trailing whitespace"}})" );

	CHECK_FALSE( prefill.matched );

	// an unrelated 400 is not a downgrade, so it stays fatal
	CHECK_FALSE( model::detect_feature_downgrade(
		R"({"error":{"message":"invalid model"}})" ).matched );
}

TEST_CASE( "the request member names are overridable per provider", "[provider]" ) {
	auto descriptor = model::descriptor_from_json( R"({
		"name": "x", "endpoint": "https://x/v1",
		"stream": { "text_delta": "/t" },
		"request": {
			"tools": "functions",
			"tool_choice": "function_call",
			"parallel_tool_calls": "parallel_calls",
			"tool_strict": "schema_strict"
		}
	})" );

	REQUIRE( static_cast< bool >( descriptor ) );
	CHECK( descriptor->request.tools == "functions" );
	CHECK( descriptor->request.tool_choice == "function_call" );
	CHECK( descriptor->request.parallel_tool_calls == "parallel_calls" );
	CHECK( descriptor->request.tool_strict == "schema_strict" );
}
