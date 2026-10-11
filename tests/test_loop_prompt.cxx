#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <string>
#include <vector>

#include "mcode/agent/loop_internal.hxx"
#include "mcode/model/render.hxx"

#include "loop_test_helpers.hxx"

using namespace mcode;
using namespace loop_test;

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

	// the seeded history is part of the fill, so the size uses the loop's own estimator
	// and the boundary cannot drift with the seed text.

	// a history long enough that a compaction has something to drop: with the pinned
	// prefix and tail covering everything, nothing is removed and the trigger is unobservable.
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

		// a compaction record means messages were removed, not merely that the threshold was crossed.
		if ( logged ) {
			CHECK( tight.history( ).size( ) < before + 2 );
		}

		return logged;
	};

	// one token either side of the boundary, measured against the seeded baseline.
	const auto baseline = loop_internal::history_tokens( seeded( ) );

	CHECK_FALSE( compacts( trigger - baseline - 1 ) );
	CHECK( compacts( trigger - baseline + 1 ) );
}

TEST_CASE( "a compaction that drops nothing is not reported as one", "[loop]" ) {
	// the pinned prefix and tail can cover the whole history, which crosses the
	// threshold without dropping anything.
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

TEST_CASE( "the pinned state sits after the system prompt and before the history", "[loop][recitation]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.loop->set_plan( "1. read the parser\n2. fix the offset bug" );
	fx.client.queue( text_response( "done" ) );

	std::ignore = fx.loop->run( "fix the parser offset" );

	REQUIRE( fx.client.call_count( ) >= 1 );

	const auto& request = fx.client.request( 0 );
	REQUIRE( request.messages.size( ) >= 3 );

	CHECK( request.messages[ 0 ].speaker == model::role::system );

	// The goal and the plan, in one block ahead of every turn the model produced.
	const auto& pinned = request.messages[ 1 ];

	REQUIRE( pinned.speaker == model::role::system );
	REQUIRE( !pinned.blocks.empty( ) );
	CHECK( pinned.blocks[ 0 ].text.find( "fix the parser offset" ) != std::string::npos );
	CHECK( pinned.blocks[ 0 ].text.find( "read the parser" ) != std::string::npos );
}

TEST_CASE( "a plan set mid-run reaches the very next request", "[loop][recitation]" ) {
	auto fx = fixture{ };
	fx.connect( );

	// The plan is recorded by a tool during dispatch and the turn that acts on it is
	// the next one, so waiting for the recitation interval would be ten steps late.
	// `echo` is already in the registry, so re-registering it keeps the definition
	// the loop dispatches against while changing what the call does.
	fx.loop->register_handler( "echo", [ &fx ]( std::string_view ) -> result< std::string > {
		fx.loop->set_plan( "the plan recorded mid-run" );

		return std::string{ "{\"ok\":true}" };
	} );

	fx.client.queue( call_response( "echo", R"({})" ) );
	fx.client.queue( call_response( "echo", R"({})" ) );
	fx.client.queue( text_response( "acted on the plan" ) );

	std::ignore = fx.loop->run( "do the thing" );

	REQUIRE( fx.client.call_count( ) >= 3 );

	// The request after the plan was recorded must carry it.
	const auto& after = fx.client.request( 2 );

	auto found = false;

	for ( const auto& message : after.messages ) {
		for ( const auto& block : message.blocks ) {
			if ( block.text.find( "the plan recorded mid-run" ) != std::string::npos ) {
				found = true;
			}
		}
	}

	CHECK( found );
}

TEST_CASE( "a run with no plan still recites the task", "[loop][recitation]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( text_response( "done" ) );

	std::ignore = fx.loop->run( "a task with no plan" );

	REQUIRE( fx.client.call_count( ) >= 1 );

	const auto& request = fx.client.request( 0 );
	REQUIRE( request.messages.size( ) >= 3 );

	CHECK( request.messages[ 1 ].speaker == model::role::system );
	CHECK( request.messages[ 1 ].blocks[ 0 ].text.find( "a task with no plan" ) != std::string::npos );
}

TEST_CASE( "the recitation survives a compaction", "[loop][recitation]" ) {
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->budget( ).max_usd = 1.0;

	fx.loop->set_plan( "keep the goal" );

	// Enough input to trip the compaction trigger on the second request.
	auto expensive = call_response( "echo", R"({})" );
	expensive.input_tokens = 900'000;
	fx.client.queue( expensive );
	fx.client.queue( call_response( "echo", R"({})" ) );
	fx.client.queue( text_response( "wrapped up" ) );

	std::ignore = fx.loop->run( "the original goal" );

	REQUIRE( fx.client.call_count( ) >= 2 );

	const auto& after = fx.client.request( fx.client.call_count( ) - 1 );
	REQUIRE( after.messages.size( ) >= 3 );
	REQUIRE( after.messages[ 1 ].speaker == model::role::system );

	const auto& block = after.messages[ 1 ].blocks[ 0 ].text;
	CHECK( block.find( "the original goal" ) != std::string::npos );
	CHECK( block.find( "keep the goal" ) != std::string::npos );
}
