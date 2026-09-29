// The ReAct loop, driven by a scripted fake client.
//
// The loop's correctness is the state machine's, not the transport's, so every
// test here drives model_client with queued events and asserts on the visited
// state sequence -- never on a network.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/support/json.hxx" 

using namespace mcode;

namespace {

	inline constexpr std::int64_t TEST_CONTEXT_WINDOW = 200'000;

	// A scripted client: one queued response per stream() call. Records every
	// request it saw, so tests can assert on call counts and on request bytes.
	class scripted_client final : public model::model_client {
	public:
		// One scripted turn: the canonical events to emit, in order.
		struct response {
			std::string text;
			std::vector< mcode::tool_call > calls;
			std::int64_t input_tokens = 100;
			std::int64_t output_tokens = 50;
			bool fail = false;
		};

		auto queue( response value ) -> void {
			responses_.push_back( std::move( value ) );
		}

		auto stream( const model::stream_request& request, const model::event_sink& sink )
			-> status override {
			++calls_;
			requests_.push_back( request.request );

			if ( index_ >= responses_.size( ) ) {
				return std::unexpected( fail( errc::protocol, "script exhausted" ) );
			}

			const auto& scripted = responses_[ index_++ ];

			if ( scripted.fail ) {
				return std::unexpected( fail( errc::protocol, "scripted hard error" ) );
			}

			if ( scripted.input_tokens > 0 ) {
				auto usage = model::chat_event{ };
				usage.type = model::chat_event::kind::usage;
				usage.input_tokens = scripted.input_tokens;
				usage.output_tokens = scripted.output_tokens;
				sink( usage );
			}

			if ( !scripted.text.empty( ) ) {
				auto delta = model::chat_event{ };
				delta.type = model::chat_event::kind::text_delta;
				delta.text = scripted.text;
				sink( delta );
			}

			for ( const auto& call : scripted.calls ) {
				auto fragment = model::chat_event{ };
				fragment.type = model::chat_event::kind::tool_call_delta;
				fragment.index = static_cast< int >( emitted_calls_ );
				fragment.tool_call_id = "call-" + std::to_string( emitted_calls_ );
				fragment.tool_name = call.name;
				fragment.args_fragment = call.args_json;
				sink( fragment );
				++emitted_calls_;
			}

			auto done = model::chat_event{ };
			done.type = model::chat_event::kind::turn_done;
			sink( done );

			return status{ };
		}

		[[nodiscard]] auto call_count( ) const noexcept -> std::size_t { return calls_; }
		[[nodiscard]] auto request( const std::size_t index ) const -> const model::chat_request& {
			return requests_[ index ];
		}

	private:
		std::vector< response > responses_;
		std::vector< model::chat_request > requests_;
		std::size_t index_ = 0;
		std::size_t calls_ = 0;
		int emitted_calls_ = 0;
	};

	auto state_names( const std::vector< loop_state >& states ) -> std::string {
		auto out = std::string{ };

		for ( const auto value : states ) {
			if ( !out.empty( ) ) {
				out += ",";
			}

			out += to_string( value );
		}

		return out;
	}

	struct fixture {
		scripted_client client;
		tool_registry registry;
		event_log log;
		agent_loop::dependencies deps;
		std::unique_ptr< agent_loop > loop;

		fixture( ) {
			deps.client = &client;
			deps.registry = &registry;
			deps.log = &log;
			deps.model_name = "test-model";
			deps.caps.context_window = TEST_CONTEXT_WINDOW;
			deps.caps.price_input = 1.0;
			deps.caps.price_output = 2.0;
			deps.caps.caching = model::cache_mode::implicit;

			auto definition = tool_def{ };
			definition.name = "echo";
			definition.description = "echoes its argument";
			definition.schema_json = R"({"type":"object"})";

			std::ignore = registry.add( definition );

			auto shell = tool_def{ };
			shell.name = "bash";
			shell.description = "runs a command";
			shell.schema_json = R"({"type":"object"})";

			std::ignore = registry.add( shell );

			loop = std::make_unique< agent_loop >( deps );
		}

		auto connect( ) -> void {
			loop->register_handler( "echo", []( std::string_view args ) -> result< std::string > {
				return std::string{ args };
			} );

			loop->register_handler( "bash", []( std::string_view args ) -> result< std::string > {
				auto command = std::string{ args };

				if ( command.find( "crash" ) != std::string::npos ) {
					return std::unexpected( fail( errc::tool_failed, "spawn failed" ) );
				}

				const auto exit_code = command.find( "pass" ) != std::string::npos ? 0 : 1;

				auto out = std::string{ "{\"ok\":true,\"exit_code\":" };
				out += std::to_string( exit_code );
				out += "}";

				return out;
			} );
		}
	};

	auto text_response( const std::string_view text ) -> scripted_client::response {
		auto value = scripted_client::response{ };
		value.text = std::string{ text };

		return value;
	}

	auto call_response( const std::string_view name, const std::string_view args )
		-> scripted_client::response {
		auto value = scripted_client::response{ };
		value.calls.push_back( mcode::tool_call{ std::string{ name }, std::string{ args } } );

		return value;
	}

} // namespace

TEST_CASE( "a plain turn reaches handoff through verify", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( text_response( "planning" ) );
	fx.client.queue( text_response( "all done" ) );

	const auto outcome = fx.loop->run( "write a hello world" );

	REQUIRE( outcome.has_value( ) );
	CHECK( state_names( outcome->visited ) == "plan,act,verify,handoff" );
	CHECK( outcome->final_state == loop_state::handoff );
	CHECK( fx.client.call_count( ) == 2 );
}

TEST_CASE( "a tool call is dispatched and observed", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( text_response( "planning" ) );
	fx.client.queue( call_response( "echo", R"({"text":"hi"})" ) );
	fx.client.queue( text_response( "done now" ) );

	const auto outcome = fx.loop->run( "echo hi then stop" );

	REQUIRE( outcome.has_value( ) );
	CHECK( state_names( outcome->visited ) == "plan,act,observe,act,verify,handoff" );
}

TEST_CASE( "thrash triggers reflect", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( text_response( "planning" ) );
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

	fx.client.queue( text_response( "planning" ) );
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

TEST_CASE( "thrash escalates to replan and then handoff", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.client.queue( text_response( "planning" ) );

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
	fx.loop->budget( ).max_steps = 3;

	fx.client.queue( text_response( "planning" ) );
	fx.client.queue( call_response( "echo", R"({})" ) );

	const auto outcome = fx.loop->run( "one call only" );

	REQUIRE( outcome.has_value( ) );
	CHECK( outcome->final_state == loop_state::handoff );
	CHECK( state_names( outcome->visited ) == "plan,act,observe,handoff" );
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

	fx.client.queue( text_response( "planning" ) );
	fx.client.queue( call_response( "echo", R"({"n":1})" ) );
	fx.client.queue( text_response( "finished" ) );

	std::ignore = fx.loop->run( "stable prefix" );

	REQUIRE( fx.client.call_count( ) == 3 );

	const auto& first = fx.client.request( 1 );
	const auto& second = fx.client.request( 2 );

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

	fx.client.queue( text_response( "planning" ) );

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

	auto assembled = assemble_request( fx.registry, prompt, { }, "test-model", fx.deps.caps,
		model::cache_mode::implicit, false, { } );

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

	const auto has_year = prompt.find( "2026" ) != std::string::npos ||
		prompt.find( "2027" ) != std::string::npos;

	CHECK( !has_year );
	CHECK( prompt.find( "session id" ) == std::string::npos );
}

TEST_CASE( "compaction triggers at 80 percent of the usable window", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );

	fx.deps.caps.context_window = 4'000;

	auto small = agent_loop::dependencies{ };
	small.client = &fx.client;
	small.registry = &fx.registry;
	small.log = &fx.log;
	small.model_name = "test-model";
	small.caps = fx.deps.caps;

	auto tight = agent_loop{ small };
	tight.register_handler( "echo", []( std::string_view args ) -> result< std::string > {
		return std::string{ args };
	} );

	fx.client.queue( text_response( "planning" ) );
	fx.client.queue( call_response( "echo", std::string( 6'000, 'x' ) ) );
	fx.client.queue( text_response( "done" ) );

	const auto outcome = tight.run( "compact me" );

	REQUIRE( outcome.has_value( ) );

	auto compacted = false;

	for ( const auto& event : fx.log.events( ) ) {
		if ( event.kind == "context.compaction" ) {
			compacted = true;
		}
	}

	CHECK( compacted );

	const auto& history = tight.history( );

	REQUIRE( !history.empty( ) );
	CHECK( history.front( ).speaker == mcode::model::role::user );
	CHECK( history.front( ).text( ).find( "compact me" ) != std::string::npos );
}

TEST_CASE( "verify with a passing command reaches done", "[loop]" ) {
	auto fx = fixture{ };
	fx.connect( );
	fx.loop->set_verification_command( "pass" );

	fx.client.queue( text_response( "planning" ) );
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

	fx.client.queue( text_response( "planning" ) );
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

	fx.client.queue( text_response( "planning" ) );
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

	fx.client.queue( text_response( "planning" ) );
	fx.client.queue( call_response( "missing_tool", R"({})" ) );
	fx.client.queue( text_response( "diagnosis: the tool name was wrong" ) );
	fx.client.queue( text_response( "final answer" ) );

	const auto outcome = fx.loop->run( "call a missing tool" );

	REQUIRE( outcome.has_value( ) );
	CHECK( state_names( outcome->visited ) ==
		"plan,act,observe,reflect,act,verify,handoff" );
}

TEST_CASE( "compaction keeps the first event and the task text", "[loop]" ) {
	auto result = compaction_result{ };
	result.pinned_facts.push_back( "the original task" );

	CHECK( !result.pinned_facts.empty( ) );
	CHECK( result.pinned_facts.front( ) == "the original task" );
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

	CHECK( budget.steps_used == 1 );
	CHECK( budget.tokens_used == 1'000 );
	CHECK( budget.usd_used == Catch::Approx( 0.10 ) );
	CHECK( !budget.nearly_exhausted( ) );

	budget.charge( 1'000, 0.85 );

	CHECK( budget.nearly_exhausted( ) );
	CHECK( !budget.exhausted( ) );

	budget.charge( 1'000, 0.06 );

	CHECK( budget.exhausted( ) );
}
