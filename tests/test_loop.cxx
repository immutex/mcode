#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/agent/loop_internal.hxx"
#include "mcode/model/capabilities.hxx"
#include "mcode/model/http_client.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/model/render.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/support/json.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/tools/register.hxx"

#include "test_scratch.hxx"

#if defined( _WIN32 )
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace mcode;

#include "loop_test_helpers.hxx"

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

TEST_CASE( "explicit-marker providers get a populated cache breakpoint", "[loop][cache]" ) {
	auto fx = fixture{ };

	const auto no_history = std::vector< model::message >{ };

	auto assembled = assemble_request( fx.registry,
		{ .system_prompt = "sys",
			.history = no_history,
			.model_name = "test-model",
			.mode = model::cache_mode::explicit_markers,
			.near_budget = false,
		} );

	REQUIRE( assembled.request.cache.breakpoints.size( ) == 1 );

	auto descriptor = model::provider_descriptor{ };
	descriptor.name = "openai-chat-completions";
	descriptor.endpoint = "https://api.example.com/v1/chat/completions";

	auto stream_request = model::stream_request{ };
	stream_request.request = assembled.request;
	stream_request.provider = descriptor;

	const auto body = model::render_request( stream_request );

	REQUIRE( static_cast< bool >( body ) );
	REQUIRE( static_cast< bool >( json::document::parse( *body ) ) );

	// the marker covers the stable prefix, so it lands on the system message itself
	CHECK( body->find( R"({"cache_control":{"type":"ephemeral"},"content":"sys","role":"system"})" )
		!= std::string::npos );

	// a provider that caches on its own, or not at all, gets no markers and no plan
	auto implicit = assembled.request;
	implicit.cache.mode = model::cache_mode::implicit;

	auto implicit_stream = stream_request;
	implicit_stream.request = implicit;

	const auto implicit_body = model::render_request( implicit_stream );
	REQUIRE( static_cast< bool >( implicit_body ) );
	CHECK( implicit_body->find( "cache_control" ) == std::string::npos );

	auto unmarked = assemble_request( fx.registry,
		{ .system_prompt = "sys",
			.history = no_history,
			.model_name = "test-model",
			.mode = model::cache_mode::implicit,
			.near_budget = false,
		} );

	CHECK( unmarked.request.cache.breakpoints.empty( ) );

	auto none = unmarked.request;
	none.cache.mode = model::cache_mode::none;

	auto none_stream = stream_request;
	none_stream.request = none;

	const auto none_body = model::render_request( none_stream );
	REQUIRE( static_cast< bool >( none_body ) );
	CHECK( none_body->find( "cache_control" ) == std::string::npos );
}

TEST_CASE( "the loop reports context usage against the model's window", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	CHECK( fx.loop->context_capacity( ) == TEST_CONTEXT_WINDOW );
	CHECK( fx.loop->context_used( ) == 0 );

	fx.client.queue( text_response( "hello" ) );

	std::ignore = fx.loop->run( "say hi" );

	// the same counter the budget charges, not a second tally
	CHECK( fx.loop->context_used( ) == fx.loop->budget( ).tokens_used.load( ) );
	CHECK( fx.loop->context_used( ) == 150 );

	// an unknown model reads 0, so a renderer omits the percentage
	auto unknown_deps = agent_loop::dependencies{ };
	unknown_deps.model_name = "no-such-model";

	const auto unknown = agent_loop{ unknown_deps };

	CHECK( unknown.context_capacity( ) == 0 );
	CHECK( unknown.context_used( ) == 0 );
}

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

TEST_CASE( "the prefix is byte-stable across turns", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( call_response( "echo", R"({"n":1})" ) );
	fx.client.queue( text_response( "finished" ) );

	std::ignore = fx.loop->run( "stable prefix" );

	REQUIRE( fx.client.call_count( ) == 2 );

	const auto& first = fx.client.request( 0 );
	const auto& second = fx.client.request( 1 );

	REQUIRE( first.messages.size( ) >= 1 );
	REQUIRE( second.messages.size( ) >= 1 );

	const auto& system_one = first.messages.front( );
	const auto& system_two = second.messages.front( );

	REQUIRE( system_one.blocks.size( ) == 1 );
	REQUIRE( system_two.blocks.size( ) == 1 );
	CHECK( system_one.blocks.front( ).text == system_two.blocks.front( ).text );
	CHECK( first.tools.size( ) == second.tools.size( ) );

	if ( first.tools.size( ) == second.tools.size( ) ) {
		for ( auto index = std::size_t{ 0 }; index < first.tools.size( ); ++index ) {
			CHECK( first.tools[ index ].name == second.tools[ index ].name );
			CHECK( first.tools[ index ].schema_json == second.tools[ index ].schema_json );
		}
	}
}

TEST_CASE( "the near-budget note is in the tail, not the prefix", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->budget( ).max_usd = 1.0;

	auto expensive_call = call_response( "echo", R"({})" );
	expensive_call.input_tokens = 900'000;
	fx.client.queue( expensive_call );

	fx.client.queue( call_response( "echo", R"({})" ) );
	fx.client.queue( text_response( "wrapped up" ) );

	std::ignore = fx.loop->run( "note test" );

	REQUIRE( fx.client.call_count( ) >= 3 );

	const auto& first = fx.client.request( 0 );
	const auto& second = fx.client.request( 2 );

	CHECK( ( first.messages.size( ) <= second.messages.size( ) ) );

	auto note_in_second = false;
	auto note_in_first = false;

	for ( const auto& message : second.messages ) {
		for ( const auto& block : message.blocks ) {
			if ( block.text.find( "budget nearly exhausted" ) != std::string::npos ) {
				note_in_second = true;
			}
		}
	}

	for ( const auto& message : first.messages ) {
		for ( const auto& block : message.blocks ) {
			if ( block.text.find( "budget nearly exhausted" ) != std::string::npos ) {
				note_in_first = true;
			}
		}
	}

	CHECK( note_in_second );
	CHECK( !note_in_first );
}

TEST_CASE( "assembly stays within the session-start budget", "[loop]" ) {
	auto fx = fixture{ };

	auto prompt = build_system_prompt( fx.registry );

	const auto words = static_cast< std::int64_t >( prompt.size( ) / CHARS_PER_TOKEN_ESTIMATE );

	CHECK( words <= SYSTEM_PROMPT_TOKEN_BUDGET );

	const auto no_history = std::vector< model::message >{ };

	auto assembled = assemble_request( fx.registry,
		{ .system_prompt = prompt,
			.history = no_history,
			.model_name = "test-model",
			.mode = model::cache_mode::implicit,
			.near_budget = false,
		} );

	auto total = std::int64_t{ 0 };

	for ( const auto& spec : assembled.request.tools ) {
		total += static_cast< std::int64_t >( spec.schema_json.size( ) / CHARS_PER_TOKEN_ESTIMATE );
	}

	CHECK( total <= TOOLS_TOKEN_BUDGET );
}

TEST_CASE( "no timestamp or cwd in the system prompt", "[loop]" ) {
	auto fx = fixture{ };

	const auto prompt = build_system_prompt( fx.registry );

	CHECK( prompt.find( "C:\\" ) == std::string::npos );

	// any build-time date would surface as a four-digit year somewhere in the prompt.
	CHECK_FALSE( std::regex_search( prompt, std::regex{ R"((19|20)[0-9]{2})" } ) );
	CHECK( prompt.find( "session id" ) == std::string::npos );
}

TEST_CASE( "compaction triggers at 80 percent of the usable window", "[loop]" ) {
	const auto usable = static_cast< double >( 20'000 - RESERVED_OUTPUT_TOKENS ) *
		( 1.0 - SAFETY_MARGIN_FRACTION );
	const auto trigger = static_cast< std::int64_t >( usable * COMPACTION_TRIGGER_FRACTION );

	// The seeded history is part of the fill, so the answer only has to make up the
	// difference. Computed with the loop's own estimator rather than a hand count,
	// or the boundary assertion would drift with the seed text.

	// A history long enough that a compaction has something to drop. Without it the
	// pinned prefix and the tail meet, cover the whole history, and nothing is
	// removed -- which is now correctly NOT reported as a compaction, so the
	// trigger would be unobservable here.
	auto seeded = []( ) {
		auto history = std::vector< model::message >{ };

		for ( auto index = 0; index < 12; ++index ) {
			auto message = model::message{ };
			message.speaker = index % 2 == 0 ? model::role::user : model::role::assistant;

			auto block = model::block{ };
			block.kind = model::block_kind::text;
			block.text = "seed message " + std::to_string( index );
			message.blocks.push_back( std::move( block ) );

			history.push_back( std::move( message ) );
		}

		return history;
	};

	auto compacts = [ & ]( const std::int64_t answer_tokens ) {
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
		tight.seed_history( seeded( ) );

		const auto before = tight.history( ).size( );
		const auto answer = std::string(
			static_cast< std::size_t >( answer_tokens ) * CHARS_PER_TOKEN_ESTIMATE, 'x' );

		fx.client.queue( text_response( answer ) );
		std::ignore = tight.run( "t" );

		auto logged = false;

		for ( const auto& event : fx.log.events( ) ) {
			if ( event.kind == "context.compaction" ) {
				logged = true;
			}
		}

		// A compaction record must mean messages were actually removed, not merely
		// that the threshold was crossed.
		if ( logged ) {
			CHECK( tight.history( ).size( ) < before + 2 );
		}

		return logged;
	};

	// One token either side of the boundary, measured against the seeded baseline
	// because the seed is already part of the fill the trigger is compared with.
	const auto baseline = loop_internal::history_tokens( seeded( ) );

	CHECK_FALSE( compacts( trigger - baseline - 1 ) );
	CHECK( compacts( trigger - baseline + 1 ) );
}

TEST_CASE( "a compaction that drops nothing is not reported as one", "[loop]" ) {
	// The pinned prefix and the tail can cover the whole history -- a couple of
	// messages with one enormous newest one, which is exactly what crosses the
	// threshold. Rebuilding the identical history and logging
	// `context.compaction` claimed a drop that did not happen.
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

	// Nothing seeded: the turn's own task and answer are all there is.
	fx.client.queue( text_response( std::string( 200'000, 'x' ) ) );
	std::ignore = tight.run( "t" );

	for ( const auto& event : fx.log.events( ) ) {
		CHECK( event.kind != "context.compaction" );
	}
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
