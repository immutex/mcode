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

	// insertion order must not leak into the bytes
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

	// the schema is embedded as parsed structure, or the provider cannot validate it.
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
	REQUIRE( body->find( R"("tool_calls":[{"function":{"arguments":"{\"path\":\"a.txt\"}","name":"read"},"id":"call_1")" )
		!= std::string::npos );
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

	// markers apply right-to-left, or the first shifts the second and the cache misses.
	const auto marker_text = std::string{ "\"cache_control\":{\"type\":\"ephemeral\"}," };
	const auto first_anchor = plain->find( "[{" );

	REQUIRE( first_anchor != std::string::npos );

	const auto second_anchor = plain->find( "},{", first_anchor );

	REQUIRE( second_anchor != std::string::npos );

	const auto first_offset = first_anchor + 2;
	const auto second_offset = second_anchor + 3;

	REQUIRE( second_offset > first_offset );

	request.cache.breakpoints = { first_offset, second_offset };

	const auto marked = model::render_chat_completions( request, descriptor.request );
	REQUIRE( static_cast< bool >( marked ) );

	REQUIRE( static_cast< bool >( json::document::parse( *marked ) ) );

	auto expected = *plain;
	expected.insert( second_offset, marker_text );
	expected.insert( first_offset, marker_text );

	REQUIRE( *marked == expected );

	auto left_to_right = *plain;
	left_to_right.insert( first_offset, marker_text );
	left_to_right.insert( second_offset, marker_text );

	REQUIRE( *marked != left_to_right );

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

TEST_CASE( "explicit markers get one breakpoint on the stable prefix when none are given",
	"[render]" ) {
	auto request = make_request( );
	request.cache.mode = model::cache_mode::explicit_markers;

	const auto descriptor = chat_completions_descriptor( );

	REQUIRE( request.cache.breakpoints.empty( ) );

	const auto offsets = model::cache_breakpoints( request, descriptor.request );

	REQUIRE( offsets.size( ) == 1 );

	// the marker lands on the system message, so the tools block before it is cached too
	auto located = request;
	located.cache.breakpoints = offsets;

	auto marked = model::render_chat_completions( located, descriptor.request );

	REQUIRE( static_cast< bool >( marked ) );
	REQUIRE( static_cast< bool >( json::document::parse( *marked ) ) );

	CHECK( marked->find( R"({"cache_control":{"type":"ephemeral"},"content":"be terse","role":"system"})" )
		!= std::string::npos );

	// and a breakpoint the caller supplies is applied verbatim
	auto manual = request;
	manual.cache.breakpoints = { offsets.front( ) };

	const auto with_offsets = model::render_chat_completions( manual, descriptor.request );
	REQUIRE( static_cast< bool >( with_offsets ) );
	CHECK( *with_offsets == *marked );

	// an empty plan renders no marker: the assembler, not the renderer, decides
	const auto unmarked = model::render_chat_completions( request, descriptor.request );
	REQUIRE( static_cast< bool >( unmarked ) );
	CHECK( unmarked->find( "cache_control" ) == std::string::npos );

	// a request that already carries offsets keeps them: a marker would shift the located one
	auto preset = request;
	preset.cache.breakpoints = { 4 };

	const auto kept = model::cache_breakpoints( preset, descriptor.request );
	REQUIRE( kept.size( ) == 1 );
	CHECK( kept.front( ) == 4 );
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
