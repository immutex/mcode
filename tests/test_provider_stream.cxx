#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "mcode/model/delta_applier.hxx"
#include "mcode/model/http_client.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/support/json.hxx"

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

	auto collect( model::delta_applier& applier, const std::string_view name, const std::string_view data )
		-> std::vector< model::chat_event > {
		auto produced = applier.feed( name, data );
		REQUIRE( produced.has_value( ) );

		return *produced;
	}

}

TEST_CASE( "a descriptor drives a real stream end to end", "[provider]" ) {
	auto applier = model::delta_applier{ gateway( ) };

	{
		const auto& d = gateway( );
		REQUIRE( d.stream.text_delta == "/choices/0/delta/content" );
		REQUIRE( d.stream.tool_call_args == "/choices/0/delta/tool_calls/0/function/arguments" );
	}

	{
		auto payload = json::document::parse( R"({"choices":[{"delta":{"content":"Hel"}}]})" );
		REQUIRE( static_cast< bool >( payload ) );
		REQUIRE( payload->has_pointer( "/choices/0/delta/content" ) );

		auto direct = payload->pointer_string( "/choices/0/delta/content" );
		REQUIRE( static_cast< bool >( direct ) );
		REQUIRE( *direct == "Hel" );
	}

	{
		auto first = collect( applier, "message", R"({"choices":[{"delta":{"content":"Hel"}}]})" );
		REQUIRE( first.size( ) == 1 );

		if ( !first.empty( ) ) {
			REQUIRE( first[ 0 ].type == model::chat_event::kind::text_delta );
			REQUIRE( first[ 0 ].text == "Hel" );
		}
	}

	auto all = std::vector< model::chat_event >{ };

	for ( const auto* payload : {
		R"({"choices":[{"delta":{"content":"Hel"}}]})",
		R"({"choices":[{"delta":{"content":"lo, "}}]})",
		R"({"choices":[{"delta":{"content":"world"}}]})" } ) {
		for ( auto& event : collect( applier, "message", payload ) ) {
			all.push_back( std::move( event ) );
		}
	}

	REQUIRE( collect( applier, "message", "" ).empty( ) );

	for ( auto& event : collect( applier, "message",
		R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","function":{"name":"read","arguments":"{\"pa"}}]}}]})" ) ) {
		all.push_back( std::move( event ) );
	}

	for ( auto& event : collect( applier, "message",
		R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"th\":\"a.tx"}}]}}]})" ) ) {
		all.push_back( std::move( event ) );
	}

	for ( auto& event : collect( applier, "message",
		R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"t\"}"}}]},"finish_reason":"tool_calls"}]})" ) ) {
		all.push_back( std::move( event ) );
	}

	for ( auto& event : collect( applier, "message",
		R"({"usage":{"prompt_tokens":120,"completion_tokens":45}})" ) ) {
		all.push_back( std::move( event ) );
	}

	REQUIRE( collect( applier, "message", "[DONE]" ).empty( ) );
	REQUIRE( applier.saw_terminal_event( ) );

	auto tail = applier.finish( );

	auto text = std::string{ };

	for ( const auto& event : all ) {
		if ( event.type == model::chat_event::kind::text_delta ) {
			text += event.text;
		}
	}

	REQUIRE( text == "Hello, world" );

	const auto* call = static_cast< const model::chat_event* >( nullptr );

	for ( const auto& event : all ) {
		if ( event.type == model::chat_event::kind::tool_call_delta && !event.args_fragment.empty( ) ) {
			call = &event;
		}
	}

	REQUIRE( call != nullptr );
	REQUIRE( call->tool_name == "read" );
	REQUIRE( call->tool_call_id == "call_a" );

	// finish() is the only point where accumulated fragments are guaranteed to be valid JSON.
	const auto* final_call = static_cast< const model::chat_event* >( nullptr );

	for ( const auto& event : tail ) {
		if ( event.type == model::chat_event::kind::tool_call_delta ) {
			final_call = &event;
		}
	}

	REQUIRE( final_call != nullptr );
	REQUIRE( final_call->tool_name == "read" );
	REQUIRE( final_call->args_fragment == R"({"path":"a.txt"})" );

	REQUIRE_FALSE( tail.empty( ) );
	REQUIRE( tail.back( ).type == model::chat_event::kind::turn_done );
	REQUIRE( tail.back( ).stop_reason == "tool_calls" );

	REQUIRE( applier.accumulated_usage( ).input == 120 );
	REQUIRE( applier.accumulated_usage( ).output == 45 );
}

TEST_CASE( "a malformed stream event surfaces rather than truncating", "[provider]" ) {
	auto applier = model::delta_applier{ gateway( ) };

	auto produced = applier.feed( "message", "{not json" );

	REQUIRE_FALSE( produced.has_value( ) );
	REQUIRE( produced.error( ).code == errc::protocol );
}

TEST_CASE( "parallel tool calls are tracked by index", "[provider]" ) {
	const auto json = R"({
		"name": "x", "endpoint": "https://x/v1",
		"stream": {
			"text_delta": "/t",
			"tool_calls": { "index": "/i", "id": "/id", "name": "/n", "args": "/a" }
		}
	})";

	auto descriptor = model::descriptor_from_json( json );
	REQUIRE( descriptor.has_value( ) );

	auto applier = model::delta_applier{ *descriptor };

	collect( applier, "", R"({"i":0,"id":"c0","n":"read","a":"{\"p\":"})" );
	collect( applier, "", R"({"i":1,"id":"c1","n":"glob","a":"{\"q\":"})" );
	collect( applier, "", R"({"i":0,"a":"\"a\"}"})" );
	collect( applier, "", R"({"i":1,"a":"\"*.cxx\"}"})" );

	const auto tail = applier.finish( );

	auto calls = std::vector< model::chat_event >{ };

	for ( const auto& event : tail ) {
		if ( event.type == model::chat_event::kind::tool_call_delta ) {
			calls.push_back( event );
		}
	}

	REQUIRE( calls.size( ) == 2 );
	REQUIRE( calls[ 0 ].index == 0 );
	REQUIRE( calls[ 0 ].tool_name == "read" );
	REQUIRE( calls[ 0 ].args_fragment == R"({"p":"a"})" );
	REQUIRE( calls[ 1 ].index == 1 );
	REQUIRE( calls[ 1 ].tool_name == "glob" );
	REQUIRE( calls[ 1 ].args_fragment == R"({"q":"*.cxx"})" );
}

TEST_CASE( "a provider that omits the tool-call index still works", "[provider]" ) {
	const auto json = R"({
		"name": "x", "endpoint": "https://x/v1",
		"stream": { "text_delta": "/t", "tool_calls": { "name": "/n", "args": "/a" } }
	})";

	auto descriptor = model::descriptor_from_json( json );
	REQUIRE( descriptor.has_value( ) );

	auto applier = model::delta_applier{ *descriptor };

	collect( applier, "", R"({"n":"read","a":"{\"p\":\"a\"}"})" );

	const auto tail = applier.finish( );
	auto found = false;

	for ( const auto& event : tail ) {
		if ( event.type == model::chat_event::kind::tool_call_delta ) {
			REQUIRE( event.index == 0 );
			REQUIRE( event.args_fragment == R"({"p":"a"})" );
			found = true;
		}
	}

	REQUIRE( found );
}

TEST_CASE( "a tool-call index off the wire is bounded", "[provider]" ) {
	// the tool-call index is stored as a bounded int.
	auto descriptor = model::descriptor_from_json( R"({
		"name": "indexed",
		"endpoint": "https://example.invalid/v1/chat/completions",
		"stream": {
			"tool_calls": {
				"index": "/index",
				"name": "/name",
				"args": "/arguments"
			}
		}
	})" );

	REQUIRE( static_cast< bool >( descriptor ) );

	if ( !descriptor ) {
		FAIL( descriptor.error( ).msg );
	}

	auto applier = model::delta_applier{ *descriptor };

	for ( const auto* payload : { R"({"index":-1,"name":"x","arguments":"{}"})",
		R"({"index":999999,"name":"x","arguments":"{}"})" } ) {
		auto produced = applier.feed( "message", payload );

		REQUIRE_FALSE( static_cast< bool >( produced ) );

		if ( !produced ) {
			REQUIRE( produced.error( ).code == errc::protocol );
		}
	}
}

TEST_CASE( "reasoning tokens are mapped, not dropped", "[provider]" ) {
	auto descriptor = model::descriptor_from_json( R"({
		"name": "reasoning",
		"endpoint": "https://example.invalid/v1/chat/completions",
		"stream": {
			"text_delta": "/choices/0/delta/content",
			"usage": {
				"in": "/usage/prompt_tokens",
				"out": "/usage/completion_tokens",
				"reasoning": "/usage/completion_tokens_details/reasoning_tokens"
			}
		}
	})" );

	REQUIRE( static_cast< bool >( descriptor ) );

	if ( !descriptor ) {
		FAIL( descriptor.error( ).msg );
	}

	auto applier = model::delta_applier{ *descriptor };

	auto produced = applier.feed( "message", R"({
		"choices": [ { "delta": { "content": "x" } } ],
		"usage": {
			"prompt_tokens": 10,
			"completion_tokens": 40,
			"completion_tokens_details": { "reasoning_tokens": 25 }
		}
	})" );

	REQUIRE( static_cast< bool >( produced ) );

	REQUIRE_FALSE( produced->empty( ) );

	auto usage_event = std::find_if( produced->begin( ), produced->end( ),
		[]( const model::chat_event& value ) {
			return value.type == model::chat_event::kind::usage;
		} );

	REQUIRE( usage_event != produced->end( ) );

	if ( usage_event != produced->end( ) ) {
		REQUIRE( usage_event->input_tokens == 10 );
		REQUIRE( usage_event->output_tokens == 40 );
		REQUIRE( usage_event->reasoning_tokens == 25 );
	}

	REQUIRE( applier.accumulated_usage( ).reasoning == 25 );
}

TEST_CASE( "a gateway that pins the tool-call index at zero does not merge distinct calls",
	"[provider]" ) {
	// ollama #15457: the index is present but constant, so keying by it merges every call.
	const auto json = R"({
		"name": "x", "endpoint": "https://x/v1",
		"stream": {
			"text_delta": "/t",
			"tool_calls": { "index": "/i", "id": "/id", "name": "/n", "args": "/a" }
		}
	})";

	auto descriptor = model::descriptor_from_json( json );
	REQUIRE( descriptor.has_value( ) );

	auto applier = model::delta_applier{ *descriptor };

	collect( applier, "", R"({"i":0,"id":"c0","n":"read","a":"{\"p\":\"a\"}"})" );
	collect( applier, "", R"({"i":0,"id":"c1","n":"glob","a":"{\"q\":\"*.cxx\"}"})" );

	auto calls = std::vector< model::chat_event >{ };

	for ( const auto& event : applier.finish( ) ) {
		if ( event.type == model::chat_event::kind::tool_call_delta ) {
			calls.push_back( event );
		}
	}

	REQUIRE( calls.size( ) == 2 );
	CHECK( calls[ 0 ].tool_name == "read" );
	CHECK( calls[ 0 ].args_fragment == R"({"p":"a"})" );
	CHECK( calls[ 1 ].tool_name == "glob" );
	CHECK( calls[ 1 ].args_fragment == R"({"q":"*.cxx"})" );
	CHECK( calls[ 0 ].index != calls[ 1 ].index );
}

TEST_CASE( "a fragment that re-sends the accumulated id replaces rather than appends",
	"[provider]" ) {
	const auto json = R"({
		"name": "x", "endpoint": "https://x/v1",
		"stream": {
			"text_delta": "/t",
			"tool_calls": { "index": "/i", "id": "/id", "name": "/n", "args": "/a" }
		}
	})";

	auto descriptor = model::descriptor_from_json( json );
	REQUIRE( descriptor.has_value( ) );

	auto applier = model::delta_applier{ *descriptor };

	// the same id arrives on both fragments, which some gateways do
	collect( applier, "", R"({"i":0,"id":"call_1","n":"read","a":"{\"p\":"})" );
	collect( applier, "", R"({"i":0,"id":"call_1","n":"read","a":"\"a\"}"})" );

	auto calls = std::vector< model::chat_event >{ };

	for ( const auto& event : applier.finish( ) ) {
		if ( event.type == model::chat_event::kind::tool_call_delta ) {
			calls.push_back( event );
		}
	}

	REQUIRE( calls.size( ) == 1 );
	CHECK( calls[ 0 ].tool_call_id == "call_1" );
	CHECK( calls[ 0 ].args_fragment == R"({"p":"a"})" );
}

TEST_CASE( "a fragment with an absent index continues the newest call", "[provider]" ) {
	const auto json = R"({
		"name": "x", "endpoint": "https://x/v1",
		"stream": {
			"text_delta": "/t",
			"tool_calls": { "id": "/id", "name": "/n", "args": "/a" }
		}
	})";

	auto descriptor = model::descriptor_from_json( json );
	REQUIRE( descriptor.has_value( ) );

	auto applier = model::delta_applier{ *descriptor };

	collect( applier, "", R"({"id":"c0","n":"read","a":"{\"p\":"})" );
	collect( applier, "", R"({"a":"\"a\"}"})" );

	auto calls = std::vector< model::chat_event >{ };

	for ( const auto& event : applier.finish( ) ) {
		if ( event.type == model::chat_event::kind::tool_call_delta ) {
			calls.push_back( event );
		}
	}

	REQUIRE( calls.size( ) == 1 );
	CHECK( calls[ 0 ].args_fragment == R"({"p":"a"})" );
}
