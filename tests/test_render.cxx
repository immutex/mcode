#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "mcode/model/render.hxx"
#include "mcode/model/types.hxx"
#include "mcode/support/json.hxx"

using namespace mcode;

namespace {

	auto text_block( const std::string_view body ) -> model::block {
		auto block = model::block{ };
		block.kind = model::block_kind::text;
		block.text = std::string{ body };

		return block;
	}

	auto chat_completions_descriptor( ) -> model::provider_descriptor {
		auto descriptor = model::provider_descriptor{ };
		descriptor.name = "openai-chat-completions";
		descriptor.endpoint = "https://api.example.com/v1/chat/completions";

		return descriptor;
	}

	auto make_request( ) -> model::chat_request {
		auto request = model::chat_request{ };
		request.model = "test-model";
		request.max_output_tokens = 4096;
		request.temperature = 0.2;

		auto system = model::message{ };
		system.speaker = model::role::system;
		system.blocks.push_back( text_block( "be terse" ) );

		auto user = model::message{ };
		user.speaker = model::role::user;
		user.blocks.push_back( text_block( "read the file" ) );

		request.messages = { system, user };

		return request;
	}

}

TEST_CASE( "render_request is byte-stable", "[render]" ) {
	auto request = make_request( );

	auto first = model::tool_spec{ };
	first.name = "write";
	first.description = "Write a file";
	first.schema_json = R"({"type":"object"})";

	auto second = model::tool_spec{ };
	second.name = "read";
	second.description = "Read a file";
	second.schema_json = R"({"type":"object"})";

	// Deliberately out of sorted order: the render must sort, so insertion
	// order must not leak into the bytes.
	request.tools = { first, second };

	const auto descriptor = chat_completions_descriptor( );
	auto one = model::stream_request{ };
	one.request = request;
	one.provider = descriptor;

	auto two = model::stream_request{ };
	two.request = request;
	two.provider = descriptor;

	const auto first_body = model::render_request( one );
	const auto second_body = model::render_request( two );

	REQUIRE( static_cast< bool >( first_body ) );
	REQUIRE( static_cast< bool >( second_body ) );
	REQUIRE( *first_body == *second_body );

	// The sort is visible in the bytes: read precedes write.
	REQUIRE( first_body->find( "\"read\"" ) < first_body->find( "\"write\"" ) );
}

TEST_CASE( "render_request omits unset fields", "[render]" ) {
	auto request = make_request( );
	request.temperature = -1.0;
	request.max_output_tokens = 0;
	request.response_schema_json.clear( );
	request.tools.clear( );

	const auto descriptor = chat_completions_descriptor( );
	const auto body = model::render_chat_completions( request, descriptor.request );

	REQUIRE( static_cast< bool >( body ) );
	REQUIRE( body->find( "temperature" ) == std::string::npos );
	REQUIRE( body->find( "max_tokens" ) == std::string::npos );
	REQUIRE( body->find( "response_format" ) == std::string::npos );
	REQUIRE( body->find( "\"tools\"" ) == std::string::npos );
	REQUIRE( body->find( "\"stream\":true" ) != std::string::npos );
}

TEST_CASE( "render_request sorts tool schemas and embeds them as JSON", "[render]" ) {
	auto request = make_request( );

	auto first = model::tool_spec{ };
	first.name = "zeta";
	first.description = "z";
	first.schema_json = R"({"type":"object","properties":{"a":{"type":"string"}}})";

	auto second = model::tool_spec{ };
	second.name = "alpha";
	second.description = "a";
	second.schema_json = R"({"type":"object"})";

	request.tools = { first, second };

	const auto descriptor = chat_completions_descriptor( );
	const auto body = model::render_chat_completions( request, descriptor.request );

	REQUIRE( static_cast< bool >( body ) );

	const auto alpha = body->find( "\"alpha\"" );
	const auto zeta = body->find( "\"zeta\"" );

	REQUIRE( alpha != std::string::npos );
	REQUIRE( zeta != std::string::npos );
	REQUIRE( alpha < zeta );

	// The schema is embedded as structure, not as a quoted string: a quoted
	// string would arrive at the provider as a schema it cannot validate.
	REQUIRE( body->find( "\\\"type\\\"" ) == std::string::npos );
	REQUIRE( body->find( "\"parameters\":{\"type\":\"object\"" ) != std::string::npos );
}

TEST_CASE( "render_request renders roles through the descriptor", "[render]" ) {
	auto request = make_request( );

	auto assistant = model::message{ };
	assistant.speaker = model::role::assistant;

	auto call = model::block{ };
	call.kind = model::block_kind::tool_call;
	call.tool_call_id = "call_1";
	call.tool_name = "read";
	call.args_json = R"({"path":"a.txt"})";

	assistant.blocks.push_back( std::move( call ) );

	auto tool = model::message{ };
	tool.speaker = model::role::tool;

	auto result = model::block{ };
	result.kind = model::block_kind::tool_result;
	result.tool_call_id = "call_1";
	result.result_json = R"({"content":"hello"})";

	tool.blocks.push_back( std::move( result ) );

	request.messages.push_back( std::move( assistant ) );
	request.messages.push_back( std::move( tool ) );

	const auto descriptor = chat_completions_descriptor( );
	const auto body = model::render_chat_completions( request, descriptor.request );

	REQUIRE( static_cast< bool >( body ) );
	REQUIRE( body->find( R"("tool_calls":[{"id":"call_1")" ) != std::string::npos );
	REQUIRE( body->find( R"("role":"tool","tool_call_id":"call_1")" ) != std::string::npos );
}

TEST_CASE( "breakpoints apply right-to-left", "[render]" ) {
	auto request = make_request( );
	request.cache.mode = model::cache_mode::explicit_markers;

	const auto descriptor = chat_completions_descriptor( );

	auto marker_free = request;
	marker_free.cache.mode = model::cache_mode::none;

	const auto plain = model::render_chat_completions( marker_free, descriptor.request );
	REQUIRE( static_cast< bool >( plain ) );
	REQUIRE( static_cast< bool >( json::document::parse( *plain ) ) );

	// Two breakpoints at distinct offsets, ordered so a left-to-right pass
	// corrupts the second one. Both offsets sit where a marker is valid JSON:
	// right after the opening brace, and right after the comma that closes the
	// first message object -- the marker's own trailing comma provides the
	// next separator.
	const auto marker_text = std::string{ "\"cache_control\":{\"type\":\"ephemeral\"}," };
	const auto first_offset = std::size_t{ 1 };
	const auto second_offset = plain->find( "}," ) + 2;

	REQUIRE( second_offset != std::string::npos );
	REQUIRE( second_offset > first_offset );

	request.cache.breakpoints = { first_offset, second_offset };

	const auto marked = model::render_chat_completions( request, descriptor.request );
	REQUIRE( static_cast< bool >( marked ) );

	// The marked body is valid JSON, which proves neither marker landed
	// inside a string or between a value and its comma.
	REQUIRE( static_cast< bool >( json::document::parse( *marked ) ) );

	// The marked body is exactly the plain body with the two markers applied
	// right to left: each offset addresses the marker-free body and is still
	// valid when it is used.
	auto expected = *plain;
	expected.insert( second_offset, marker_text );
	expected.insert( first_offset, marker_text );

	REQUIRE( *marked == expected );

	// A left-to-right pass produces different bytes: after the first marker
	// shifts everything, the second original offset no longer addresses the
	// intended boundary. The silent cache miss is the failure mode this
	// ordering rule exists to prevent.
	auto left_to_right = *plain;
	left_to_right.insert( first_offset, marker_text );
	left_to_right.insert( second_offset, marker_text );

	REQUIRE( *marked != left_to_right );

	// Removing both markers recovers the plain body byte for byte.
	auto recovered = *marked;
	auto erased = std::size_t{ 0 };

	while ( true ) {
		const auto position = recovered.find( marker_text );

		if ( position == std::string::npos ) {
			break;
		}

		recovered.erase( position, marker_text.size( ) );
		++erased;
	}

	REQUIRE( erased == 2 );
	REQUIRE( recovered == *plain );
}

TEST_CASE( "implicit cache mode renders no markers", "[render]" ) {
	auto request = make_request( );
	request.cache.mode = model::cache_mode::implicit;
	request.cache.breakpoints = { 10, 20 };

	const auto descriptor = chat_completions_descriptor( );
	const auto body = model::render_chat_completions( request, descriptor.request );
	const auto plain = model::render_chat_completions( request, descriptor.request );

	REQUIRE( static_cast< bool >( body ) );
	REQUIRE( body->find( "cache_control" ) == std::string::npos );
	REQUIRE( *body == *plain );
}

TEST_CASE( "an offset past the body is an error, not a silent clamp", "[render]" ) {
	auto request = make_request( );
	request.cache.mode = model::cache_mode::explicit_markers;
	request.cache.breakpoints = { 100'000 };

	const auto descriptor = chat_completions_descriptor( );
	const auto body = model::render_chat_completions( request, descriptor.request );

	REQUIRE_FALSE( static_cast< bool >( body ) );
	REQUIRE( body.error( ).msg.find( "past the rendered body" ) != std::string::npos );
}

TEST_CASE( "an unrenderable request shape is refused by name", "[render]" ) {
	auto descriptor = model::provider_descriptor{ };
	descriptor.name = "anthropic-messages";
	descriptor.endpoint = "https://api.anthropic.com/v1/messages";

	REQUIRE_FALSE( model::is_renderable_shape( descriptor ) );
	REQUIRE( model::is_renderable_shape( chat_completions_descriptor( ) ) );
}

TEST_CASE( "render_request refuses an unrenderable descriptor by name", "[render]" ) {
	auto descriptor = model::provider_descriptor{ };
	descriptor.name = "anthropic-messages";
	descriptor.endpoint = "https://api.anthropic.com/v1/messages";

	auto request = make_request( );

	auto stream_request = model::stream_request{ };
	stream_request.request = request;
	stream_request.provider = descriptor;

	const auto refused = model::render_request( stream_request );

	REQUIRE_FALSE( static_cast< bool >( refused ) );
	REQUIRE( refused.error( ).code == errc::unsupported );
	REQUIRE( refused.error( ).msg.find( "anthropic-messages" ) != std::string::npos );
	REQUIRE( refused.error( ).msg.find( "chat-completions" ) != std::string::npos );
}
