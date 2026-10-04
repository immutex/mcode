#include <catch2/catch_test_macros.hpp>

#include <string>
#include <tuple>

#include "loop_test_helpers.hxx"

using namespace mcode;
using namespace loop_test;

TEST_CASE( "a reflect response's calls are never orphaned by a replan", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	// the model answers two reflects in a row with calls instead of a diagnosis.
	for ( auto round = 0; round < 5; ++round ) {
		fx.client.queue( call_response( "echo", R"({"a":1})" ) );
	}

	fx.client.queue( text_response( "new plan" ) );
	fx.client.queue( text_response( "final answer" ) );

	const auto outcome = fx.loop->run( "keep calling" );

	REQUIRE( outcome.has_value( ) );

	const auto path = state_names( outcome->visited );

	CHECK( path.find( "reflect" ) != std::string::npos );
	CHECK( path.find( "replan" ) != std::string::npos );

	auto calls = std::size_t{ 0 };
	auto results = std::size_t{ 0 };

	for ( const auto& message : fx.loop->history( ) ) {
		for ( const auto& block : message.blocks ) {
			if ( block.kind == model::block_kind::tool_call ) {
				++calls;
			}

			if ( block.kind == model::block_kind::tool_result ) {
				++results;
			}
		}
	}

	// a tool_call without its result is rejected by the provider on the next request.
	CHECK( calls > 0 );
	CHECK( calls == results );
}

TEST_CASE( "compaction keeps the newest message and answers every call", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	auto small = agent_loop::dependencies{ };
	small.client = &fx.client;
	small.registry = &fx.registry;
	small.log = &fx.log;
	small.model_name = "test-model";
	small.caps = fx.deps.caps;
	small.caps.context_window = 20'000;

	auto tight = agent_loop{ small };

	tight.register_handler( "echo", []( std::string_view args ) -> result< std::string > {
		return std::string{ args };
	} );

	// the second result alone exceeds the tail budget, so the cut lands inside a tool pair.
	fx.client.queue( call_response( "echo", R"({"n":1})" ) );
	fx.client.queue( call_response( "echo", std::string( 80'004, 'x' ) ) );
	fx.client.queue( text_response( "done" ) );

	const auto outcome = tight.run( "compact between calls" );

	REQUIRE( outcome.has_value( ) );

	const auto& history = tight.history( );

	REQUIRE( !history.empty( ) );

	// the answer just produced is kept even though it follows a message over the budget.
	CHECK( history.back( ).text( ) == "done" );

	auto calls = std::size_t{ 0 };
	auto results = std::size_t{ 0 };

	for ( const auto& message : history ) {
		for ( const auto& block : message.blocks ) {
			if ( block.kind == model::block_kind::tool_call ) {
				++calls;
			}

			if ( block.kind == model::block_kind::tool_result ) {
				++results;
			}
		}
	}

	CHECK( calls > 0 );
	CHECK( calls == results );
}
