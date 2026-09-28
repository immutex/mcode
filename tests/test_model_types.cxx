#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

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

TEST_CASE( "a request survives a JSON round-trip", "[model]" ) {
	// D1's acceptance: the canonical types must serialize and parse back without
	// loss. This is what proves a request can be logged, replayed, or sent over
	// the extension boundary as plain data (docs/19 §Extension host interface).
	const auto original = make_request( );

	auto doc = json::document::make_object( );
	REQUIRE( static_cast< bool >( doc.set_string( "model", original.model ) ) );
	REQUIRE( static_cast< bool >( doc.set_int( "max_output_tokens", original.max_output_tokens ) ) );
	REQUIRE( static_cast< bool >( doc.set_string( "reasoning_effort",
		original.reasoning_effort == model::effort::medium ? "medium" : "other" ) ) );
	REQUIRE( static_cast< bool >( doc.set_string( "response_schema",
		original.response_schema_json ) ) );

	auto serialised = doc.dump( );
	REQUIRE( static_cast< bool >( serialised ) );

	auto reparsed = json::document::parse( *serialised );
	REQUIRE( static_cast< bool >( reparsed ) );

	auto model_name = reparsed->get_string( "model" );
	REQUIRE( model_name.has_value( ) );
	REQUIRE( *model_name == original.model );

	auto tokens = reparsed->get_int( "max_output_tokens" );
	REQUIRE( tokens.has_value( ) );
	REQUIRE( *tokens == original.max_output_tokens );

	auto schema = reparsed->get_string( "response_schema" );
	REQUIRE( schema.has_value( ) );
	REQUIRE( *schema == original.response_schema_json );

	// The messages themselves must be JSON-safe: every field a provider needs to
	// reconstruct a turn is plain data.
	REQUIRE( original.messages.size( ) == 4 );
	REQUIRE( original.messages[ 2 ].blocks[ 0 ].args_json == R"({"path":"a.txt"})" );
	REQUIRE( original.messages[ 3 ].blocks[ 0 ].result_json == R"({"content":"hello"})" );

	auto block_doc = json::document::parse( original.messages[ 2 ].blocks[ 0 ].args_json );
	REQUIRE( static_cast< bool >( block_doc ) );
	auto path = block_doc->get_string( "path" );
	REQUIRE( path.has_value( ) );
	REQUIRE( *path == "a.txt" );
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

TEST_CASE( "an incomplete tool call is empty args, never partial JSON", "[model]" ) {
	// docs/15: fragments arrive split arbitrarily, so accumulating into a string
	// that is re-parsed per event would fail on every event but the last. The
	// canonical form holds fragments, not partial JSON.
	auto event = model::chat_event{ };
	event.type = model::chat_event::kind::tool_call_delta;
	event.index = 0;
	event.tool_call_id = "call_7";
	event.tool_name = "write";
	event.args_fragment = R"({"ci)";

	REQUIRE( event.args_fragment == R"({"ci)" );

	auto block_value = model::block{ };
	block_value.kind = model::block_kind::tool_call;
	block_value.tool_call_id = "call_7";

	// Until complete, args_json is empty and must not be parsed as JSON.
	REQUIRE( block_value.args_json.empty( ) );
	REQUIRE_FALSE( static_cast< bool >( json::document::parse( block_value.args_json ) ) );
}
