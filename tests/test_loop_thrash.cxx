#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "loop_test_helpers.hxx"

using namespace mcode;
using namespace loop_test;

TEST_CASE( "thrash triggers reflect", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( call_response( "echo", R"({"a":1})" ) );
	fx.client.queue( call_response( "echo", R"({"a":1})" ) );
	fx.client.queue( call_response( "echo", R"({"a":1})" ) );
	fx.client.queue( text_response( "diagnosis: stop calling echo" ) );
	fx.client.queue( text_response( "I will stop now" ) );

	const auto outcome = fx.loop->run( "loop forever" );

	REQUIRE( outcome.has_value( ) );
	INFO( state_names( outcome->visited ) );
	CHECK( state_names( outcome->visited ) ==
		"plan,act,observe,act,observe,act,observe,reflect,act,verify,handoff" );
}

TEST_CASE( "key-order-only differences still thrash", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( call_response( "echo", R"({"a":1,"b":2})" ) );
	fx.client.queue( call_response( "echo", R"({"b":2,"a":1})" ) );
	fx.client.queue( call_response( "echo", R"({"a":1,"b":2})" ) );
	fx.client.queue( text_response( "diagnosis: stop calling echo" ) );
	fx.client.queue( text_response( "I will stop now" ) );

	const auto outcome = fx.loop->run( "loop forever" );

	REQUIRE( outcome.has_value( ) );
	INFO( state_names( outcome->visited ) );
	CHECK( state_names( outcome->visited ) ==
		"plan,act,observe,act,observe,act,observe,reflect,act,verify,handoff" );
}

TEST_CASE( "a third identical call is refused before it runs", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	auto payloads = std::vector< std::string >{ };
	std::ignore = fx.loop->bus( ).subscribe( events::kind::tool_result,
		[ &payloads ]( const events::event& value ) { payloads.push_back( value.payload_json ); } );

	fx.client.queue( call_response( "echo", R"({"a":1})" ) );
	fx.client.queue( call_response( "echo", R"({"a":1})" ) );
	fx.client.queue( call_response( "echo", R"({"a":1})" ) );
	fx.client.queue( text_response( "diagnosis: stop repeating" ) );
	fx.client.queue( call_response( "echo", R"({"a":2})" ) );
	fx.client.queue( text_response( "final answer" ) );

	const auto outcome = fx.loop->run( "repeat until refused" );

	REQUIRE( outcome.has_value( ) );

	auto results = std::vector< std::string >{ };

	for ( const auto& message : fx.loop->history( ) ) {
		for ( const auto& block : message.blocks ) {
			if ( block.kind == model::block_kind::tool_result ) {
				results.push_back( block.result_json );
			}
		}
	}

	REQUIRE( results.size( ) == 4 );

	// the first two identical calls reach the handler, which echoes its arguments
	CHECK( results[ 0 ].find( R"("a":1)" ) != std::string::npos );
	CHECK( results[ 1 ].find( R"("a":1)" ) != std::string::npos );

	// the third is refused instead, and the refusal names the tool and the repeat count
	CHECK( results[ 2 ].find( "doom loop" ) != std::string::npos );
	CHECK( results[ 2 ].find( "echo" ) != std::string::npos );
	CHECK( results[ 2 ].find( "already been dispatched 2 times" ) != std::string::npos );
	CHECK( results[ 2 ].find( R"("retryable":true)" ) != std::string::npos );

	// different arguments are not a repeat, so that call still runs
	CHECK( results[ 3 ].find( R"("a":2)" ) != std::string::npos );

	const auto refusals = std::count( payloads.begin( ), payloads.end( ),
		R"({"ok":false,"error":"doom_loop","tool":"echo"})" );

	CHECK( refusals == 1 );
}

TEST_CASE( "thrash escalates to replan and then handoff", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	for ( auto round = 0; round < 3; ++round ) {
		fx.client.queue( call_response( "echo", R"({"a":1})" ) );
	}

	fx.client.queue( text_response( "diagnosis: the approach is wrong" ) );

	for ( auto round = 0; round < 3; ++round ) {
		fx.client.queue( call_response( "echo", R"({"a":1})" ) );
	}

	fx.client.queue( text_response( "new plan: try something else" ) );
	fx.client.queue( text_response( "final answer" ) );

	const auto outcome = fx.loop->run( "loop forever" );

	REQUIRE( outcome.has_value( ) );

	const auto path = state_names( outcome->visited );

	INFO( path );

	CHECK( path.find( "reflect" ) != std::string::npos );
	CHECK( path.find( "replan" ) != std::string::npos );
	CHECK( outcome->final_state == loop_state::handoff );
}

TEST_CASE( "thrash detector counts repeats and respects the window", "[loop]" ) {
	auto detector = thrash_detector{ };

	CHECK( detector.record( "echo", R"({"a":1})" ) == 1 );
	CHECK( detector.record( "echo", R"({"a":1})" ) == 2 );
	CHECK( detector.record( "echo", R"({"b":2})" ) == 1 );
	CHECK( detector.record( "echo", R"({"a":1})" ) == 1 );

	CHECK( detector.record( "echo", R"({"a": 1})" ) == 2 );
}

TEST_CASE( "canonicalize sorts object keys", "[json]" ) {
	auto canonical = json::canonicalize( R"({"z":1,"a":2})" );

	REQUIRE( canonical.has_value( ) );

	const auto sorted = canonical->find( "\"a\":2,\"z\":1" ) != std::string::npos;

	CHECK( sorted );
}

TEST_CASE( "session budget charges tokens and usd together", "[loop]" ) {
	auto budget = session_budget{ };
	budget.max_steps = 10;
	budget.max_usd = 1.0;

	budget.charge( 1'000, 0.10 );

	CHECK( budget.steps_used.load( ) == 1 );
	CHECK( budget.tokens_used.load( ) == 1'000 );
	CHECK( budget.usd_used.load( ) == Catch::Approx( 0.10 ) );
	CHECK( !budget.nearly_exhausted( ) );

	budget.charge( 1'000, 0.85 );

	CHECK( budget.nearly_exhausted( ) );
	CHECK( !budget.exhausted( ) );

	budget.charge( 1'000, 0.06 );

	CHECK( budget.exhausted( ) );
}
