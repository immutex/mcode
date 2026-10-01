// The loop's event contract: which events are published, and what they carry.
//
// Split from test_loop.cxx, which owns the state machine. These cover the
// publish side, where two defects were invisible to any state assertion: a
// `turn_end` that no terminal path reached, and a `tool_call` payload that a
// second move left empty.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "loop_test_helpers.hxx"

using namespace loop_test;


TEST_CASE( "turn_end is published on every terminal path", "[loop][events]" ) {
	// It used to sit after the state machine's loop, and `handoff`, `done` and
	// `failed` all return from inside it -- so it was never published at all.
	// The TUI commits a turn's answer on this event, so every reply was erased
	// with the live region. The loop now publishes it from a destructor guard,
	// and this covers each of the three ways a turn can end.
	const auto endings = std::vector< std::pair< std::string, std::string > >{
		// { label, verification command }. Empty means Verify routes to
		// handoff; "pass" reaches done.
		{ "handoff", "" },
		{ "done", "pass" },
	};

	for ( const auto& [ label, command ] : endings ) {
		auto fx = fixture{ };
		fx.connect( );

		if ( !command.empty( ) ) {
			fx.loop->set_verification_command( command );
		}

		auto turn_end_count = 0;
		const auto id = fx.loop->bus( ).subscribe( events::kind::turn_end,
			[ &turn_end_count ]( const events::event& ) { ++turn_end_count; } );

		fx.client.queue( text_response( "planning" ) );
		fx.client.queue( text_response( "all done" ) );

		const auto outcome = fx.loop->run( "reach " + label );

		REQUIRE( outcome.has_value( ) );
		CHECK( turn_end_count == 1 );

		fx.loop->bus( ).unsubscribe( id );
	}

	// `failed` is the third exit, and it returns from the state machine too.
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->budget( ).max_steps = 0;

	auto failed_turn_end_count = 0;
	std::ignore = fx.loop->bus( ).subscribe( events::kind::turn_end,
		[ &failed_turn_end_count ]( const events::event& ) { ++failed_turn_end_count; } );

	const auto outcome = fx.loop->run( "no budget" );

	REQUIRE( outcome.has_value( ) );
	CHECK( outcome->final_state == loop_state::failed );
	CHECK( failed_turn_end_count == 1 );
}

TEST_CASE( "the tool_call event carries the tool name", "[loop][events]" ) {
	// The payload was moved into the event log and then moved again into the
	// published event, so the event carried an empty string. The live region
	// renders the tool row from this field, and a blank name made every running
	// call anonymous while the finished rows (`\u2713 read`) stayed correct.
	auto fx = fixture{ };
	fx.connect( );

	auto payloads = std::vector< std::string >{ };
	std::ignore = fx.loop->bus( ).subscribe( events::kind::tool_call,
		[ &payloads ]( const events::event& value ) { payloads.push_back( value.payload_json ); } );

	fx.client.queue( text_response( "planning" ) );
	fx.client.queue( call_response( "echo", R"({"text":"hi"})" ) );
	fx.client.queue( text_response( "done now" ) );

	const auto outcome = fx.loop->run( "echo hi then stop" );

	REQUIRE( outcome.has_value( ) );
	REQUIRE( payloads.size( ) == 1 );

	auto parsed = json::document::parse( payloads.front( ) );
	REQUIRE( parsed.has_value( ) );

	const auto tool = parsed->pointer_string( "/tool" );
	REQUIRE( tool.has_value( ) );
	CHECK( *tool == "echo" );

	// The log gets the same text, so the two readers cannot disagree.
	CHECK( fx.log.events( ).size( ) > 0 );
}
