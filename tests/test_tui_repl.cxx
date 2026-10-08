#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/cli/repl.hxx"
#include "mcode/perm/approval.hxx"
#include "mcode/tui/approval_tui.hxx"

#include "loop_test_helpers.hxx"
#include "permission_test_helpers.hxx"

using namespace mcode;
using namespace mcode::tui;
using namespace loop_test;

namespace {

	class multi_turn_client final : public model::model_client {
	public:
		auto queue( scripted_client::response value ) -> void {
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

		[[nodiscard]] auto request( const std::size_t index ) const
			-> const model::chat_request& {
			return requests_[ index ];
		}

	private:
		std::vector< scripted_client::response > responses_;
		std::vector< model::chat_request > requests_;
		std::size_t index_ = 0;
		std::size_t calls_ = 0;
		int emitted_calls_ = 0;
	};

}

TEST_CASE( "two turns in one process keep the history and a follow-up sees the "
	"first turn's tool results", "[tui][repl]" ) {
	auto client = multi_turn_client{ };
	auto registry = tool_registry{ };
	auto log = event_log{ };

	// the definition must be in the registry, not just the handler, or dispatch fails
	auto definition = tool_def{ };
	definition.name = "echo";
	definition.description = "echoes its argument";
	definition.schema_json = R"({"type":"object"})";

	std::ignore = registry.add( definition );

	auto deps = agent_loop::dependencies{ };
	deps.client = &client;
	deps.registry = &registry;
	deps.log = &log;
	deps.model_name = "test-model";
	deps.caps.context_window = TEST_CONTEXT_WINDOW;

	auto loop = agent_loop{ deps };
	loop.register_handler( "echo", []( std::string_view args ) -> result< std::string > {
		return std::string{ args };
	} );

	auto turn = cli::session{ loop };

	client.queue( call_response( "echo", R"({"note":"turn-one-result"})" ) );
	client.queue( text_response( "first turn done" ) );

	const auto first = turn.run_turn( "do the first thing" );
	REQUIRE( first == cli::exit_code::success );

	const auto history_after_first = turn.history( ).size( );
	REQUIRE( history_after_first > 0 );

	client.queue( text_response( "follow-up done" ) );

	const auto second = turn.run_turn( "what did the first turn find" );
	REQUIRE( second == cli::exit_code::success );

	REQUIRE( client.call_count( ) == 3 );

	const auto& follow_up = client.request( 2 );

	auto saw_result = false;

	for ( const auto& message : follow_up.messages ) {
		for ( const auto& block : message.blocks ) {
			if ( block.kind == model::block_kind::tool_result &&
				block.result_json.find( "turn-one-result" ) != std::string::npos ) {
				saw_result = true;
			}
		}
	}

	CHECK( saw_result );

	CHECK( turn.history( ).size( ) > history_after_first );
	CHECK( turn.history( ).front( ).text( ) == "do the first thing" );
}

TEST_CASE( "the loop's run() resets per-run state, not the history", "[tui][repl]" ) {
	auto client = multi_turn_client{ };
	auto registry = tool_registry{ };
	auto log = event_log{ };

	auto definition = tool_def{ };
	definition.name = "echo";
	definition.description = "echoes its argument";
	definition.schema_json = R"({"type":"object"})";

	std::ignore = registry.add( definition );

	auto deps = agent_loop::dependencies{ };
	deps.client = &client;
	deps.registry = &registry;
	deps.log = &log;
	deps.model_name = "test-model";
	deps.caps.context_window = TEST_CONTEXT_WINDOW;

	auto loop = agent_loop{ deps };

	client.queue( text_response( "one" ) );
	const auto first = loop.run( "first" );
	REQUIRE( first.has_value( ) );

	const auto after_first = loop.history( ).size( );

	client.queue( text_response( "two" ) );
	const auto second = loop.run( "second" );
	REQUIRE( second.has_value( ) );

	// the reset makes the second run walk the whole path again, from plan
	CHECK( second->visited == first->visited );
	CHECK( second->visited.front( ) == loop_state::plan );

	CHECK( loop.history( ).size( ) > after_first );
	CHECK( loop.history( ).front( ).text( ) == "first" );
	CHECK( loop.history( )[ 2 ].text( ) == "second" );
}

TEST_CASE( "the session exit code is the last turn's code", "[tui][repl]" ) {
	auto client = multi_turn_client{ };
	auto registry = tool_registry{ };
	auto log = event_log{ };

	auto deps = agent_loop::dependencies{ };
	deps.client = &client;
	deps.registry = &registry;
	deps.log = &log;
	deps.model_name = "test-model";
	deps.caps.context_window = TEST_CONTEXT_WINDOW;
	deps.budget.max_steps = 1;

	auto loop = agent_loop{ deps };

	client.queue( text_response( "ok" ) );

	auto lines = std::deque< std::optional< std::string > >{ };
	lines.push_back( std::string{ "only turn" } );
	lines.push_back( std::string{ "second turn that fails" } );
	lines.push_back( std::nullopt );

	const auto input = [ &lines ]( ) -> std::optional< std::string > {
		if ( lines.empty( ) ) {
			return std::nullopt;
		}

		auto value = lines.front( );
		lines.pop_front( );

		return value;
	};

	const auto code = cli::run_session( { }, input,
		[ & ]( const mcode::cli::exec_options&, perm::approval_source* )
			-> result< agent_loop > {
			return std::move( loop );
		} );

	// The second turn runs with the step budget already spent, so the documented
	// exit is 3 (budget exhausted), not 4. `docs/22` states it: "3 | Budget
	// exhausted (steps/tokens/USD) — partial state preserved, resumable". The
	// loop reports a spent budget from its Plan state as `failed`, and checking
	// the state before the flag classified it as a provider error -- so a
	// resumable budget stop told a CI script the provider had broken.
	CHECK( code == mcode::cli::to_int( mcode::cli::exit_code::budget_exhausted ) );
}

TEST_CASE( "the ui approval source keeps the engine's semantics", "[tui][approval]" ) {
	SECTION( "[?] calls the detail callback and re-prompts" ) {
		auto answers = ui_approval_source::answer_queue{ "?", "y" };
		auto source = ui_approval_source{ answers };

		auto request = perm::approval_request{ };
		request.action = "run";
		request.subject = "git status";
		request.rule = "default: exec";

		auto detail_calls = 0;

		const auto outcome = source.ask( request, [ &detail_calls ]( ) {
			++detail_calls;

			return std::string{ "detail text" };
		} );

		CHECK( outcome == perm::approval_outcome::allow_once );
		CHECK( detail_calls == 1 );
		CHECK( source.asks( ) == 1 );
		CHECK( source.details_shown( ) == 1 );
	}

	SECTION( "an unrecognised answer re-prompts" ) {
		auto answers = ui_approval_source::answer_queue{ "maybe", "n" };
		auto source = ui_approval_source{ answers };

		auto request = perm::approval_request{ };
		request.action = "run";
		request.subject = "git push";
		request.rule = "default: exec";

		const auto outcome = source.ask( request, [ ]( ) { return std::string{ }; } );

		CHECK( outcome == perm::approval_outcome::deny_once );
		CHECK( source.asks( ) == 1 );
	}

	SECTION( "closed input is refused, which the engine resolves as deny" ) {
		auto answers = ui_approval_source::answer_queue{ };
		auto source = ui_approval_source{ answers };

		auto request = perm::approval_request{ };
		request.action = "run";
		request.subject = "rm -rf /";
		request.rule = "default: exec";

		const auto outcome = source.ask( request, [ ]( ) { return std::string{ }; } );

		CHECK( outcome == perm::approval_outcome::refused );
	}

	SECTION( "always answers as allow_remember" ) {
		auto answers = ui_approval_source::answer_queue{ "a" };
		auto source = ui_approval_source{ answers };

		auto request = perm::approval_request{ };
		request.action = "run";
		request.subject = "npm install";
		request.rule = "default: exec";

		const auto outcome = source.ask( request, [ ]( ) { return std::string{ }; } );

		CHECK( outcome == perm::approval_outcome::allow_remember );
	}
}

TEST_CASE( "an allow_remember answer survives into the next turn without "
	"re-prompting", "[tui][repl][approval]" ) {
	auto space = workspace::open( test::scratch_directory( "mcode-repl-perm" ) );
	REQUIRE( space.has_value( ) );

	auto store = perm::remember_store{
		test::scratch_directory( "mcode-repl-store" ) / "permissions.json" };
	auto engine = perm::permission_engine{ *space, &store };

	auto source = permission_test::scripted_source{ };
	source.queue( perm::approval_outcome::allow_remember );

	engine.set_approval_source( &source );

	auto request = perm::permission_request{ };
	request.tool_name = "bash";
	request.klass = tool_class::exec;
	request.resource = "turn-one-command";

	CHECK( engine.decide( request ) == perm::permission_decision::allow );
	CHECK( source.asks( ) == 1 );

	CHECK( engine.decide( request ) == perm::permission_decision::allow );
	CHECK( source.asks( ) == 1 );
}

TEST_CASE( "a turn that hands off on a failed model call is not a success",
	"[tui][repl]" ) {
	auto client = multi_turn_client{ };
	auto registry = tool_registry{ };
	auto log = event_log{ };

	auto definition = tool_def{ };
	definition.name = "echo";
	definition.description = "echoes its argument";
	definition.schema_json = R"({"type":"object"})";

	std::ignore = registry.add( definition );

	auto deps = agent_loop::dependencies{ };
	deps.client = &client;
	deps.registry = &registry;
	deps.log = &log;
	deps.model_name = "test-model";
	deps.caps.context_window = TEST_CONTEXT_WINDOW;

	auto loop = agent_loop{ deps };
	loop.register_handler( "echo", []( std::string_view args ) -> result< std::string > {
		return std::string{ args };
	} );

	auto turn = cli::session{ loop };

	// The plan asks for a tool; the request that follows the tool result finds the script
	// exhausted, so the loop hands off with a reason rather than finishing the turn.
	client.queue( call_response( "echo", R"({"note":"only one call"})" ) );

	const auto code = turn.run_turn( "do something" );

	CHECK( code == cli::exit_code::provider_error );
	CHECK_FALSE( loop.budget( ).exhausted( ) );
	CHECK_FALSE( loop.permission_denied( ) );
}
