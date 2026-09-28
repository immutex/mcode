#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/model/delta_applier.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/support/json.hxx"

using namespace mcode;

namespace {

	// The descriptor from docs/26, as an extension would register it. This is the
	// fixture the acceptance criterion names: a descriptor must drive a real
	// stream end-to-end.
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

TEST_CASE( "a descriptor drives a real stream end to end", "[provider]" ) {
	auto applier = model::delta_applier{ gateway( ) };

	{
		const auto& d = gateway( );
		REQUIRE( d.stream.text_delta == "/choices/0/delta/content" );
		REQUIRE( d.stream.tool_call_args == "/choices/0/delta/tool_calls/0/function/arguments" );
	}

	// Debug: is the pointer resolving at all?
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

	// Text arrives split across events, as providers do.
	for ( const auto* payload : {
		R"({"choices":[{"delta":{"content":"Hel"}}]})",
		R"({"choices":[{"delta":{"content":"lo, "}}]})",
		R"({"choices":[{"delta":{"content":"world"}}]})" } ) {
		for ( auto& event : collect( applier, "message", payload ) ) {
			all.push_back( std::move( event ) );
		}
	}

	// A keep-alive comment arrives as an empty payload and produces nothing.
	REQUIRE( collect( applier, "message", "" ).empty( ) );

	// Tool-call arguments split arbitrarily mid-token, which is the case that
	// breaks any implementation that parses per event.
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

	// The terminal sentinel.
	REQUIRE( collect( applier, "message", "[DONE]" ).empty( ) );
	REQUIRE( applier.saw_terminal_event( ) );

	auto tail = applier.finish( );

	// --- assertions on what the stream produced ---
	auto text = std::string{ };

	for ( const auto& event : all ) {
		if ( event.type == model::chat_event::kind::text_delta ) {
			text += event.text;
		}
	}

	REQUIRE( text == "Hello, world" );

	// The tool call must be reconstructed whole, with JSON that parses.
	const auto* call = static_cast< const model::chat_event* >( nullptr );

	for ( const auto& event : all ) {
		if ( event.type == model::chat_event::kind::tool_call_delta && !event.args_fragment.empty( ) ) {
			call = &event;
		}
	}

	REQUIRE( call != nullptr );
	REQUIRE( call->tool_name == "read" );
	REQUIRE( call->tool_call_id == "call_a" );

	// finish() reports the complete call, which is the only point at which the
	// accumulated fragments are guaranteed to be valid JSON.
	const auto* final_call = static_cast< const model::chat_event* >( nullptr );

	for ( const auto& event : tail ) {
		if ( event.type == model::chat_event::kind::tool_call_delta ) {
			final_call = &event;
		}
	}

	REQUIRE( final_call != nullptr );
	REQUIRE( final_call->tool_name == "read" );
	REQUIRE( final_call->args_fragment == R"({"path":"a.txt"})" );

	// The tail ends with turn_done carrying the stop reason.
	REQUIRE_FALSE( tail.empty( ) );
	REQUIRE( tail.back( ).type == model::chat_event::kind::turn_done );
	REQUIRE( tail.back( ).stop_reason == "tool_calls" );

	// Usage accumulated from provider-reported values.
	REQUIRE( applier.accumulated_usage( ).input == 120 );
	REQUIRE( applier.accumulated_usage( ).output == 45 );
}

TEST_CASE( "a descriptor that cannot work is rejected at load", "[provider]" ) {
	// Each of these would otherwise fail at first token, or worse, silently.
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

	// And a malformed one is a JSON error, not a crash.
	CHECK_FALSE( model::descriptor_from_json( "{not json" ).has_value( ) );
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

	// Two interleaved calls, which is what a parallel tool-call stream looks like.
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
	// Some gateways never send more than one call and omit the field entirely.
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
