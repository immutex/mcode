#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "loop_test_helpers.hxx"

using namespace loop_test;


TEST_CASE( "turn_end is published on every terminal path", "[loop][events]" ) {
	// turn_end is published from a destructor guard so all three terminal exits reach it
	const auto endings = std::vector< std::pair< std::string, std::string > >{
		// an empty command routes verify to handoff, `pass` reaches done
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

	CHECK( fx.log.events( ).size( ) > 0 );
}
