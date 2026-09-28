#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "mcode/model/delta_applier.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/model/types.hxx"
#include "mcode/support/json.hxx"

using namespace mcode;

namespace {

	// model::block has eight fields, so a designated initializer must name them in
	// declaration order and may not omit any under -Wmissing-designated-field-
	// initializers. Construct and assign instead.
	auto text_block( const std::string_view body ) -> model::block {
		auto block = model::block{ };
		block.kind = model::block_kind::text;
		block.text = std::string{ body };

		return block;
	}

}

namespace {

	auto make_request( ) -> model::chat_request {
		auto request = model::chat_request{ };
		request.model = "test-model";
		request.max_output_tokens = 4096;
		request.temperature = 0.2;
		request.reasoning_effort = model::effort::medium;
		request.response_schema_json = R"({"type":"object"})";

		auto system = model::message{ };
		system.speaker = model::role::system;
		system.blocks.push_back( text_block( "be terse" ) );

		auto user = model::message{ };
		user.speaker = model::role::user;
		user.blocks.push_back( text_block( "read the file" ) );

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

		request.messages = { system, user, assistant, tool };

		auto spec = model::tool_spec{ };
		spec.name = "read";
		spec.description = "Read a file";
		spec.schema_json = R"({"type":"object","properties":{"path":{"type":"string"}}})";
		request.tools.push_back( spec );

		request.choice.kind = model::tool_choice_kind::specific;
		request.choice.name = "read";
		request.cache.mode = model::cache_mode::explicit_markers;
		request.cache.breakpoints = { 128, 4096 };

		return request;
	}

}

TEST_CASE( "role names round-trip", "[model]" ) {
	for ( const auto value : { model::role::system, model::role::user, model::role::assistant,
		model::role::tool } ) {
		const auto text = model::to_string( value );
		auto parsed = model::role_from_string( text );

		REQUIRE( parsed.has_value( ) );
		REQUIRE( *parsed == value );
	}

	REQUIRE_FALSE( model::role_from_string( "wizard" ).has_value( ) );
	REQUIRE_FALSE( model::role_from_string( "" ).has_value( ) );
}

TEST_CASE( "the canonical request is plain data end to end", "[model]" ) {
	// D1's acceptance is that a request can cross a boundary as plain data. There
	// is no `chat_request` serializer yet, so this does NOT round-trip the type --
	// asserting that would need the serializer to exist. What it does assert is
	// the property that makes one possible: every field is a string, a number, a
	// bool or a list, and the two fields that hold JSON hold JSON that parses.
	const auto original = make_request( );

	REQUIRE( original.messages.size( ) == 4 );
	REQUIRE_FALSE( original.model.empty( ) );
	REQUIRE( original.max_output_tokens > 0 );

	// Every role a message carries has a wire name, which is what a serializer
	// would key on. The names are the ones the parser accepts.
	REQUIRE( model::role_from_string( "system" ) == model::role::system );
	REQUIRE( model::role_from_string( "user" ) == model::role::user );
	REQUIRE( model::role_from_string( "assistant" ) == model::role::assistant );
	REQUIRE( model::role_from_string( "tool" ) == model::role::tool );

	for ( const auto& message : original.messages ) {
		auto named = model::to_string( message.speaker );

		REQUIRE_FALSE( named.empty( ) );
		REQUIRE( model::role_from_string( named ) == message.speaker );
	}

	// The embedded JSON fields must actually be JSON. A tool call whose arguments
	// are not parseable is the failure a serializer would surface much later, at
	// the provider, as an opaque 400.
	auto call_args = json::document::parse( original.messages[ 2 ].blocks[ 0 ].args_json );
	REQUIRE( static_cast< bool >( call_args ) );

	if ( call_args ) {
		auto path = call_args->get_string( "path" );
		REQUIRE( path.has_value( ) );
		REQUIRE( *path == "a.txt" );
	}

	auto result_body = json::document::parse( original.messages[ 3 ].blocks[ 0 ].result_json );
	REQUIRE( static_cast< bool >( result_body ) );

	auto schema = json::document::parse( original.response_schema_json );
	REQUIRE( static_cast< bool >( schema ) );

	REQUIRE( original.tools.size( ) == 1 );
	REQUIRE( static_cast< bool >( json::document::parse( original.tools[ 0 ].schema_json ) ) );
}

TEST_CASE( "usage takes the maximum, never the sum", "[model]" ) {
	// Providers report usage either as per-event deltas or as a final cumulative
	// total. Accumulating would double-count the second kind, so the rule is
	// max-of-observed.
	auto counts = model::usage{ };

	auto first = model::chat_event{ };
	first.type = model::chat_event::kind::usage;
	first.input_tokens = 100;
	first.output_tokens = 10;
	counts.add( first );

	REQUIRE( counts.input == 100 );
	REQUIRE( counts.output == 10 );

	auto final_event = model::chat_event{ };
	final_event.type = model::chat_event::kind::usage;
	final_event.input_tokens = 100;
	final_event.output_tokens = 250;
	counts.add( final_event );

	REQUIRE( counts.input == 100 );
	REQUIRE( counts.output == 250 );

	// A later, smaller report must not lower the total.
	auto stale = model::chat_event{ };
	stale.input_tokens = 40;
	counts.add( stale );

	REQUIRE( counts.input == 100 );
}

TEST_CASE( "cost is computed from reported usage only", "[model]" ) {
	auto caps = model::capabilities{ };
	caps.model = "test-model";
	caps.price_input = 3.0;
	caps.price_cached_read = 0.3;
	caps.price_cache_write = 3.75;
	caps.price_output = 15.0;

	auto counts = model::usage{ };
	counts.input = 1'000'000;
	counts.cached_read = 800'000;
	counts.cache_write = 100'000;
	counts.output = 200'000;

	// Billed input is input minus the cached portion, which is priced separately.
	const auto expected = ( 200'000 * 3.0 + 800'000 * 0.3 + 100'000 * 3.75 + 200'000 * 15.0 ) / 1e6;
	const auto actual = model::compute_cost( caps, counts );

	REQUIRE( actual == Catch::Approx( expected ) );

	// 200k billed input @ $3 + 800k cached @ $0.30 + 100k cache write @ $3.75
	// + 200k output @ $15, per million tokens.
	REQUIRE( actual == Catch::Approx( 4.215 ) );
}

TEST_CASE( "an unknown model prices at zero rather than guessing", "[model]" ) {
	const auto caps = model::capabilities{ };
	auto counts = model::usage{ };
	counts.input = 1'000'000;
	counts.output = 1'000'000;

	REQUIRE( model::compute_cost( caps, counts ) == 0.0 );
}

TEST_CASE( "message text concatenates only textual blocks", "[model]" ) {
	auto message = model::message{ };
	message.speaker = model::role::assistant;
	auto thinking = model::block{ };
	thinking.kind = model::block_kind::thinking;
	thinking.text = "hmm ";

	message.blocks.push_back( std::move( thinking ) );
	message.blocks.push_back( text_block( "answer" ) );
	auto call = model::block{ };
	call.kind = model::block_kind::tool_call;
	call.tool_name = "read";
	call.args_json = "{}";

	message.blocks.push_back( std::move( call ) );
	auto result = model::block{ };
	result.kind = model::block_kind::tool_result;
	result.result_json = "{}";

	message.blocks.push_back( std::move( result ) );

	REQUIRE( message.text( ) == "hmm answer" );
}

TEST_CASE( "arguments split across events are not parsed per fragment", "[model]" ) {
	// Fragments arrive split arbitrarily, so a parser that ran per event would
	// reject every event but the last. The applier is what concatenates, and the
	// observable rule is that a fragment which is not valid JSON does NOT make
	// feed() fail -- only the accumulation is JSON, and only at finish().
	auto descriptor = model::descriptor_from_json( R"({
		"name": "splitter",
		"endpoint": "https://example.invalid/v1/chat/completions",
		"stream": {
			"tool_calls": {
				"index": "/index",
				"id": "/id",
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

	// Two events, each carrying half of one JSON object. Neither half parses on
	// its own -- that is the whole point -- so the payload is built through the
	// JSON API rather than by hand. Handing the fragment to a hand-written
	// template puts its own quotes into the wire text and produces malformed
	// JSON, which would test the template instead of the applier.
	for ( const auto* fragment : { R"({"ci)", R"(ty":"Paris"})" } ) {
		auto event = json::document::make_object( );

		REQUIRE( static_cast< bool >( event.set_int( "index", 0 ) ) );
		REQUIRE( static_cast< bool >( event.set_string( "id", "call_7" ) ) );
		REQUIRE( static_cast< bool >( event.set_string( "name", "write" ) ) );
		REQUIRE( static_cast< bool >( event.set_string( "arguments", fragment ) ) );

		auto payload = event.dump( );
		REQUIRE( static_cast< bool >( payload ) );

		auto produced = applier.feed( "message", *payload );

		REQUIRE( static_cast< bool >( produced ) );
	}

	// finish() emits the completed call with the fragments joined, and only then
	// is the result parseable.
	auto completed = applier.finish( );
	REQUIRE_FALSE( completed.empty( ) );

	auto call = std::find_if( completed.begin( ), completed.end( ),
		[]( const model::chat_event& value ) {
			return value.type == model::chat_event::kind::tool_call_delta &&
				!value.tool_name.empty( );
		} );

	REQUIRE( call != completed.end( ) );

	if ( call != completed.end( ) ) {
		REQUIRE( call->args_fragment == R"({"city":"Paris"})" );
		REQUIRE( static_cast< bool >( json::document::parse( call->args_fragment ) ) );
	}
}
