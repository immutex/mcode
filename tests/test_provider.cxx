#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "mcode/model/delta_applier.hxx"
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

TEST_CASE( "the escape hatch replaces the mapping entirely", "[provider]" ) {
	auto descriptor = model::descriptor_from_json( R"({
		"name": "hatch",
		"endpoint": "https://example.invalid/v1/messages",
		"on_event": true
	})" );

	REQUIRE( static_cast< bool >( descriptor ) );

	if ( !descriptor ) {
		FAIL( descriptor.error( ).msg );
	}

	auto applier = model::delta_applier{ *descriptor };

	// a declared-but-uninstalled hatch fails closed; falling through would accept every event.
	auto unmapped = applier.feed( "message", R"({"delta":"x"})" );

	REQUIRE_FALSE( static_cast< bool >( unmapped ) );

	if ( !unmapped ) {
		REQUIRE( unmapped.error( ).code == errc::protocol );
		REQUIRE( unmapped.error( ).msg.find( "escape hatch" ) != std::string::npos );
	}

	auto seen_name = std::string{ };
	auto seen_data = std::string{ };

	applier.set_escape_hatch( [&]( const std::string_view event_name, const std::string_view data )
		-> result< std::vector< model::chat_event > > {
		seen_name = std::string{ event_name };
		seen_data = std::string{ data };

		auto event = model::chat_event{ };
		event.type = model::chat_event::kind::text_delta;
		event.text = "from the hatch";

		return std::vector< model::chat_event >{ event };
	} );

	auto produced = applier.feed( "custom.event", R"({"anything":true})" );

	REQUIRE( static_cast< bool >( produced ) );
	REQUIRE( seen_name == "custom.event" );
	REQUIRE( seen_data == R"({"anything":true})" );

	if ( produced ) {
		REQUIRE( produced->size( ) == 1 );
		REQUIRE( ( *produced )[ 0 ].text == "from the hatch" );
	}
}

TEST_CASE( "a pointer gated on the event name is not applied to other events", "[provider]" ) {
	// responses puts text and tool args at the same /delta pointer, gated by SSE event name.
	auto descriptor = model::descriptor_from_json( R"({
		"name": "gated",
		"endpoint": "https://example.invalid/v1/responses",
		"stream": {
			"text_delta": "/delta",
			"text_events": ["response.output_text.delta"],
			"tool_calls": { "id": "/item_id", "args": "/delta" },
			"tool_call_events": ["response.function_call_arguments.delta"]
		}
	})" );

	REQUIRE( static_cast< bool >( descriptor ) );

	if ( !descriptor ) {
		FAIL( descriptor.error( ).msg );
	}

	auto applier = model::delta_applier{ *descriptor };

	auto text = applier.feed( "response.output_text.delta", R"({"delta":"hello"})" );

	REQUIRE( static_cast< bool >( text ) );

	if ( text ) {
		REQUIRE( text->size( ) == 1 );
		REQUIRE( ( *text )[ 0 ].type == model::chat_event::kind::text_delta );
		REQUIRE( ( *text )[ 0 ].text == "hello" );
	}

	auto args = applier.feed( "response.function_call_arguments.delta",
		R"({"item_id":"call_1","delta":"{\"a\":1}"})" );

	REQUIRE( static_cast< bool >( args ) );

	if ( args ) {
		for ( const auto& event : *args ) {
			REQUIRE( event.type != model::chat_event::kind::text_delta );
		}
	}

	auto completed = applier.finish( );

	auto call = std::find_if( completed.begin( ), completed.end( ),
		[]( const model::chat_event& value ) {
			return value.type == model::chat_event::kind::tool_call_delta;
		} );

	REQUIRE( call != completed.end( ) );

	if ( call != completed.end( ) ) {
		REQUIRE( call->args_fragment == R"({"a":1})" );
		REQUIRE( call->tool_call_id == "call_1" );
	}
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
