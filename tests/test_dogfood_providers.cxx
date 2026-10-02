#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/model/delta_applier.hxx"
#include "mcode/model/provider.hxx"

using namespace mcode;

namespace {

	// these mirror extensions/providers/init.luau; keep them in sync.
	const char* OPENAI_CHAT = R"({
		"name": "openai-chat-completions",
		"endpoint": "https://api.openai.com/v1/chat/completions",
		"auth": { "from": "env", "name": "OPENAI_API_KEY", "header": "Authorization" },
		"stream": {
			"text_delta": "/choices/0/delta/content",
			"tool_calls": {
				"index": "/choices/0/delta/tool_calls/0/index",
				"id": "/choices/0/delta/tool_calls/0/id",
				"name": "/choices/0/delta/tool_calls/0/function/name",
				"args": "/choices/0/delta/tool_calls/0/function/arguments"
			},
			"finish": "/choices/0/finish_reason",
			"usage": {
				"in": "/usage/prompt_tokens",
				"out": "/usage/completion_tokens",
				"cached_read": "/usage/prompt_tokens_details/cached_tokens"
			}
		}
	})";

	const char* OPENAI_RESPONSES = R"({
		"name": "openai-responses",
		"endpoint": "https://api.openai.com/v1/responses",
		"auth": { "from": "env", "name": "OPENAI_API_KEY", "header": "Authorization" },
		"stream": {
			"text_delta": "/delta",
			"text_events": ["response.output_text.delta"],
			"tool_calls": {
				"id": "/item_id",
				"args": "/delta"
			},
			"tool_call_events": ["response.function_call_arguments.delta"],
			"terminal_events": ["response.completed", "response.failed", "response.incomplete"]
		}
	})";

	const char* ANTHROPIC = R"({
		"name": "anthropic-messages",
		"endpoint": "https://api.anthropic.com/v1/messages",
		"auth": { "from": "env", "name": "ANTHROPIC_API_KEY", "header": "x-api-key", "scheme": "" },
		"stream": {
			"text_delta": "/delta/text",
			"thinking_delta": "/delta/thinking",
			"tool_calls": {
				"index": "/index",
				"id": "/content_block/id",
				"name": "/content_block/name",
				"args": "/delta/partial_json"
			},
			"finish": "/delta/stop_reason",
			"usage": {
				"in": "/message/usage/input_tokens",
				"out": "/usage/output_tokens",
				"cached_read": "/message/usage/cache_read_input_tokens",
				"cache_write": "/message/usage/cache_creation_input_tokens"
			},
			"terminal_events": ["message_stop"]
		}
	})";

	auto load( const char* text ) -> model::provider_descriptor {
		auto descriptor = model::descriptor_from_json( text );

		if ( !descriptor ) {
			FAIL( "descriptor rejected: " << descriptor.error( ).msg );
		}

		return *descriptor;
	}

	auto drive( model::delta_applier& applier,
		const std::vector< std::pair< const char*, const char* > >& events )
		-> std::vector< model::chat_event > {
		auto all = std::vector< model::chat_event >{ };

		for ( const auto& [ name, data ] : events ) {
			auto produced = applier.feed( name, data );

			if ( !produced ) {
				FAIL( "event rejected: " << produced.error( ).msg );
			}

			for ( auto& event : *produced ) {
				all.push_back( std::move( event ) );
			}
		}

		for ( auto& event : applier.finish( ) ) {
			all.push_back( std::move( event ) );
		}

		return all;
	}

	auto text_of( const std::vector< model::chat_event >& events ) -> std::string {
		auto out = std::string{ };

		for ( const auto& event : events ) {
			if ( event.type == model::chat_event::kind::text_delta ) {
				out += event.text;
			}
		}

		return out;
	}

	auto last_call( const std::vector< model::chat_event >& events )
		-> const model::chat_event* {
		const auto* found = static_cast< const model::chat_event* >( nullptr );

		for ( const auto& event : events ) {
			if ( event.type == model::chat_event::kind::tool_call_delta && !event.tool_name.empty( ) ) {
				found = &event;
			}
		}

		return found;
	}

}

TEST_CASE( "all three reference descriptors validate", "[dogfood]" ) {
	for ( const auto* text : { OPENAI_CHAT, ANTHROPIC, OPENAI_RESPONSES } ) {
		auto descriptor = model::descriptor_from_json( text );

		if ( !descriptor ) {
			FAIL( "reference descriptor rejected: " << descriptor.error( ).msg );
		}

		REQUIRE( !descriptor->name.empty( ) );
	}
}

TEST_CASE( "openai chat completions drives a full stream", "[dogfood]" ) {
	auto applier = model::delta_applier{ load( OPENAI_CHAT ) };

	const auto events = drive( applier, {
		{ "message", R"({"choices":[{"delta":{"role":"assistant","content":""}}]})" },
		{ "message", R"({"choices":[{"delta":{"content":"I'll read "}}]})" },
		{ "message", R"({"choices":[{"delta":{"content":"that file."}}]})" },
		{ "message", R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_9","type":"function","function":{"name":"read","arguments":""}}]}}]})" },
		{ "message", R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"pa"}}]}}]})" },
		{ "message", R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"th\":\"a.txt\"}"}}]}}]})" },
		{ "message", R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})" },
		{ "message", R"({"choices":[],"usage":{"prompt_tokens":1200,"completion_tokens":48,"prompt_tokens_details":{"cached_tokens":1024}}})" },
		{ "message", "[DONE]" },
	} );

	REQUIRE( text_of( events ) == "I'll read that file." );

	const auto* call = last_call( events );
	REQUIRE( call != nullptr );
	REQUIRE( call->tool_name == "read" );
	REQUIRE( call->tool_call_id == "call_9" );
	REQUIRE( call->args_fragment == R"({"path":"a.txt"})" );

	REQUIRE( applier.accumulated_usage( ).input == 1200 );
	REQUIRE( applier.accumulated_usage( ).output == 48 );

	REQUIRE( applier.accumulated_usage( ).cached_read == 1024 );
}

TEST_CASE( "an event arriving after the terminal is ignored", "[dogfood]" ) {
	auto applier = model::delta_applier{ load( OPENAI_CHAT ) };

	const auto events = drive( applier, {
		{ "message", R"({"choices":[{"delta":{"content":"done"}}]})" },
		{ "message", "[DONE]" },
		{ "message", R"({"choices":[{"delta":{"content":" late"}}],"usage":{"prompt_tokens":99,"completion_tokens":99}})" },
	} );

	REQUIRE( text_of( events ) == "done" );
	REQUIRE( applier.accumulated_usage( ).input == 0 );
	REQUIRE( applier.accumulated_usage( ).output == 0 );
}

TEST_CASE( "a mid-stream provider error is surfaced, not dropped", "[dogfood]" ) {
	// a provider reports failure as an ordinary event on a 200 stream.
	const auto descriptor = load( R"({
		"name": "error-reporting",
		"endpoint": "https://example.invalid/v1/chat",
		"auth": { "from": "env", "name": "EXAMPLE_API_KEY", "header": "Authorization" },
		"stream": {
			"text_delta": "/choices/0/delta/content",
			"error": { "message": "/error/message", "code": "/error/code" }
		}
	})" );

	auto applier = model::delta_applier{ descriptor };

	const auto produced = applier.feed( "message",
		R"({"error":{"message":"upstream overloaded","code":"overloaded_error"}})" );

	REQUIRE_FALSE( static_cast< bool >( produced ) );
	CHECK( produced.error( ).msg.find( "upstream overloaded" ) != std::string::npos );
	CHECK( produced.error( ).msg.find( "overloaded_error" ) != std::string::npos );
}

TEST_CASE( "anthropic drives text, thinking, and tool input", "[dogfood]" ) {
	auto applier = model::delta_applier{ load( ANTHROPIC ) };

	const auto events = drive( applier, {
		{ "message_start", R"({"type":"message_start","message":{"usage":{"input_tokens":800,"cache_read_input_tokens":600,"cache_creation_input_tokens":100}}})" },
		{ "content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking"}})" },
		{ "content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Let me check."}})" },
		{ "content_block_stop", R"({"type":"content_block_stop","index":0})" },
		{ "content_block_start", R"({"type":"content_block_start","index":1,"content_block":{"type":"text"}})" },
		{ "content_block_delta", R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Reading it now."}})" },
		{ "content_block_stop", R"({"type":"content_block_stop","index":1})" },
		{ "content_block_start", R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"toolu_1","name":"read"}})" },
		{ "content_block_delta", R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})" },
		{ "content_block_delta", R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"\"a.txt\"}"}})" },
		{ "content_block_stop", R"({"type":"content_block_stop","index":2})" },
		{ "message_delta", R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":77}})" },
		{ "message_stop", R"({"type":"message_stop"})" },
	} );

	REQUIRE( text_of( events ) == "Reading it now." );

	auto thinking = std::string{ };

	for ( const auto& event : events ) {
		if ( event.type == model::chat_event::kind::thinking_delta ) {
			thinking += event.text;
		}
	}

	REQUIRE( thinking == "Let me check." );

	const auto* call = last_call( events );
	REQUIRE( call != nullptr );
	REQUIRE( call->tool_name == "read" );
	REQUIRE( call->tool_call_id == "toolu_1" );
	REQUIRE( call->args_fragment == R"({"path":"a.txt"})" );

	// usage is max-of-observed, so cumulative and per-event reports both work.
	REQUIRE( applier.accumulated_usage( ).input == 800 );
	REQUIRE( applier.accumulated_usage( ).output == 77 );
	REQUIRE( applier.accumulated_usage( ).cached_read == 600 );
	REQUIRE( applier.accumulated_usage( ).cache_write == 100 );

	REQUIRE( applier.saw_terminal_event( ) );
}

TEST_CASE( "an anthropic message_stop ends the stream without a [DONE]", "[dogfood]" ) {
	// the terminal marker differs per provider, so it is a descriptor field.
	auto applier = model::delta_applier{ load( ANTHROPIC ) };

	REQUIRE_FALSE( applier.saw_terminal_event( ) );

	auto produced = applier.feed( "message_stop", R"({"type":"message_stop"})" );
	REQUIRE( static_cast< bool >( produced ) );
	REQUIRE( produced->empty( ) );
	REQUIRE( applier.saw_terminal_event( ) );
}

TEST_CASE( "a non-streaming error body is not mistaken for a delta", "[dogfood]" ) {
	auto applier = model::delta_applier{ load( OPENAI_CHAT ) };

	auto produced = applier.feed( "message", R"({"error":{"message":"rate limited","type":"rate_limit_error"}})" );

	REQUIRE( static_cast< bool >( produced ) );
	REQUIRE( produced->empty( ) );
}
