#include <catch2/catch_test_macros.hpp>

#include "loop_test_helpers.hxx"

using namespace mcode;
using namespace loop_test;

TEST_CASE( "a tool-free turn is one request: plan, act, verify, handoff", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( text_response( "hello there" ) );

	const auto outcome = fx.loop->run( "say hello" );

	REQUIRE( outcome.has_value( ) );
	CHECK( state_names( outcome->visited ) == "plan,act,verify,handoff" );
	CHECK( outcome->final_state == loop_state::handoff );

	// the greeting must not pay for a second full-prefix request
	CHECK( fx.client.call_count( ) == 1 );

	REQUIRE_FALSE( fx.loop->history( ).empty( ) );
	CHECK( fx.loop->history( ).back( ).text( ) == "hello there" );
}

TEST_CASE( "a tool call is dispatched and observed", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( call_response( "echo", R"({"text":"hi"})" ) );
	fx.client.queue( text_response( "done now" ) );

	const auto outcome = fx.loop->run( "echo hi then stop" );

	REQUIRE( outcome.has_value( ) );
	CHECK( state_names( outcome->visited ) == "plan,act,observe,act,verify,handoff" );
	CHECK( fx.client.call_count( ) == 2 );

	// the call reached the handler and its result is in the history the model sees
	auto saw_result = false;

	for ( const auto& message : fx.loop->history( ) ) {
		for ( const auto& block : message.blocks ) {
			if ( block.kind == model::block_kind::tool_result
				&& block.result_json.find( "hi" ) != std::string::npos ) {
				saw_result = true;
			}
		}
	}

	CHECK( saw_result );
}

TEST_CASE( "the loop reports context usage against the model's window", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	CHECK( fx.loop->context_capacity( ) == TEST_CONTEXT_WINDOW );
	CHECK( fx.loop->context_used( ) == 0 );

	fx.client.queue( text_response( "hello" ) );

	std::ignore = fx.loop->run( "say hi" );

	// window occupancy is the prompt of the last request, not prompt plus completion:
	// counting the completion makes the meter a running total of everything generated.
	CHECK( fx.loop->context_used( ) == 100 );
	CHECK( fx.loop->tokens_spent( ) == fx.loop->budget( ).tokens_used.load( ) );

	// an unknown model reads 0, so a renderer omits the percentage
	auto unknown_deps = agent_loop::dependencies{ };
	unknown_deps.model_name = "no-such-model";

	const auto unknown = agent_loop{ unknown_deps };

	CHECK( unknown.context_capacity( ) == 0 );
	CHECK( unknown.context_used( ) == 0 );
}

TEST_CASE( "budget exhaustion lands in handoff, not failed", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->budget( ).max_steps = 1;

	// the call is answered with a budget error before Observe finds the budget gone.
	fx.client.queue( call_response( "echo", R"({})" ) );

	const auto outcome = fx.loop->run( "one call only" );

	REQUIRE( outcome.has_value( ) );
	CHECK( outcome->final_state == loop_state::handoff );
	CHECK( state_names( outcome->visited ) == "plan,act,observe,handoff" );
	CHECK( fx.client.call_count( ) == 1 );
}

TEST_CASE( "failed is reachable only from plan", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->budget( ).max_steps = 0;

	const auto outcome = fx.loop->run( "no budget at all" );

	REQUIRE( outcome.has_value( ) );
	CHECK( state_names( outcome->visited ) == "plan,failed" );
	CHECK( outcome->final_state == loop_state::failed );
}

TEST_CASE( "termination is checked before the call", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->budget( ).max_steps = 1;

	fx.client.queue( text_response( "done" ) );

	std::ignore = fx.loop->run( "tiny budget" );

	CHECK( fx.client.call_count( ) == 1 );
}

TEST_CASE( "the summary names goal, actions, last failure, budget", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->budget( ).max_steps = 0;

	const auto outcome = fx.loop->run( "impossible task" );

	REQUIRE( outcome.has_value( ) );

	auto summary = std::string{ };
	auto seen_end = false;

	for ( const auto& event : fx.log.events( ) ) {
		if ( event.kind == "run.end" ) {
			seen_end = true;
			summary = event.payload_json;
		}
	}

	REQUIRE( seen_end );
	CHECK( summary.find( "\"goal\":\"impossible task\"" ) != std::string::npos );
	CHECK( summary.find( "\"actions\":" ) != std::string::npos );
	CHECK( summary.find( "\"remaining_steps\":" ) != std::string::npos );
	CHECK( summary.find( "\"state\":\"failed\"" ) != std::string::npos );
}

TEST_CASE( "verify with a passing command reaches done", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->set_verification_command( "pass" );

	fx.client.queue( text_response( "work complete" ) );

	const auto outcome = fx.loop->run( "finish with a check" );

	REQUIRE( outcome.has_value( ) );
	CHECK( state_names( outcome->visited ) == "plan,act,verify,done" );
	CHECK( outcome->final_state == loop_state::done );
}

TEST_CASE( "verify with a failing command reflects then hands off", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->set_verification_command( "fail" );

	fx.client.queue( text_response( "work complete" ) );
	fx.client.queue( text_response( "diagnosis: the test is right, the code is wrong" ) );
	fx.client.queue( text_response( "another attempt" ) );
	fx.client.queue( text_response( "diagnosis: still failing" ) );
	fx.client.queue( text_response( "final answer" ) );

	const auto outcome = fx.loop->run( "finish with a failing check" );

	REQUIRE( outcome.has_value( ) );
	CHECK( state_names( outcome->visited ) ==
		"plan,act,verify,reflect,act,verify,reflect,act,verify,reflect,replan,handoff" );
}

TEST_CASE( "a failing exit code with a successful tool call does not reach done", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->set_verification_command( "fail" );

	fx.client.queue( text_response( "work complete" ) );
	fx.client.queue( text_response( "diagnosis: the command failed" ) );
	fx.client.queue( text_response( "final answer" ) );

	const auto outcome = fx.loop->run( "failing gate must not pass" );

	REQUIRE( outcome.has_value( ) );
	CHECK( outcome->final_state != loop_state::done );
	CHECK( state_names( outcome->visited ).find( "done" ) == std::string::npos );
}

TEST_CASE( "a hard tool error routes to reflect", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( call_response( "missing_tool", R"({})" ) );
	fx.client.queue( text_response( "diagnosis: the tool name was wrong" ) );
	fx.client.queue( text_response( "final answer" ) );

	const auto outcome = fx.loop->run( "call a missing tool" );

	REQUIRE( outcome.has_value( ) );
	CHECK( state_names( outcome->visited ) ==
		"plan,act,observe,reflect,act,verify,handoff" );
}

TEST_CASE( "a completed run's log carries the same summary a failed one does", "[loop]" ) {
	// every path must write the same `run.end` fields, so a summary reader always
	// sees cost and token accounting.
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( text_response( "all done" ) );

	const auto outcome = fx.loop->run( "do the thing" );

	REQUIRE( static_cast< bool >( outcome ) );

	auto found = false;

	for ( const auto& event : fx.log.events( ) ) {
		if ( event.kind != "run.end" ) {
			continue;
		}

		found = true;

		const auto parsed = json::document::parse( event.payload_json );

		REQUIRE( static_cast< bool >( parsed ) );

		// The fields a consumer reads, present on a run that did not give up.
		CHECK( parsed->pointer_string( "/reason" ).has_value( ) );
		CHECK( parsed->pointer_string( "/state" ).has_value( ) );
		CHECK( parsed->pointer_string( "/goal" ).has_value( ) );
		CHECK( parsed->pointer_int( "/actions" ).has_value( ) );
		CHECK( parsed->pointer_int( "/input_tokens" ).has_value( ) );
		CHECK( parsed->pointer_int( "/output_tokens" ).has_value( ) );
	}

	CHECK( found );
}
