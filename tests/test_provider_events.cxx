#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "mcode/model/delta_applier.hxx"
#include "mcode/model/provider.hxx"

using namespace mcode;

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
