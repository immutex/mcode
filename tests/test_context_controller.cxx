#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/agent/context_controller.hxx"
#include "mcode/model/types.hxx"

using namespace mcode;

namespace {

	auto text_message( const model::role speaker, const std::string& text ) {
		auto message = model::message{ };
		message.speaker = speaker;

		auto block = model::block{ };
		block.kind = model::block_kind::text;
		block.text = text;
		message.blocks.push_back( std::move( block ) );

		return message;
	}

	auto tool_result_message( const std::size_t chars ) {
		auto message = model::message{ };
		message.speaker = model::role::tool;

		auto block = model::block{ };
		block.kind = model::block_kind::tool_result;
		block.result_json = std::string( chars, 'x' );
		message.blocks.push_back( std::move( block ) );

		return message;
	}

	auto occupancy( const std::int64_t prompt, const std::int64_t window ) {
		auto value = context_occupancy{ };
		value.prompt_tokens = prompt;
		value.usable_window = window;

		return value;
	}

}

TEST_CASE( "fill_fraction is the window ratio, not cumulative spend", "[context]" ) {
	const auto half = occupancy( 5000, 10000 );

	REQUIRE( half.fill_fraction( ) == 0.5 );

	const auto over = occupancy( 40000, 10000 );

	REQUIRE( over.fill_fraction( ) == 4.0 );
}

TEST_CASE( "an unknown window never reports as full", "[context]" ) {
	const auto unknown = occupancy( 5000, 0 );

	REQUIRE( unknown.fill_fraction( ) == 0.0 );
	REQUIRE( unknown.cache_hit_ratio( ) == 0.0 );
}

TEST_CASE( "below the trigger nothing is planned", "[context]" ) {
	auto history = std::vector< model::message >{ };

	for ( auto index = 0; index < 20; ++index ) {
		history.push_back( tool_result_message( 4000 ) );
	}

	const auto plan = plan_context( history, occupancy( 1000, 10000 ), 2000 );

	REQUIRE( plan.action == context_action::none );
}

TEST_CASE( "a shallow reclaimable span is not worth the cache invalidation", "[context]" ) {
	// Fill is over the trigger, but only one small result is reclaimable: the
	// rewrite would cost more than it saves, so the plan declines.
	auto history = std::vector< model::message >{ };

	for ( auto index = 0; index < 8; ++index ) {
		history.push_back( text_message( model::role::user, std::string( 400, 'y' ) ) );
	}

	history.push_back( tool_result_message( 400 ) );

	const auto plan = plan_context( history, occupancy( 9000, 10000 ), 100 );

	REQUIRE( plan.action == context_action::none );
}

TEST_CASE( "a deep reclaimable span is cleared", "[context]" ) {
	auto history = std::vector< model::message >{ };

	for ( auto index = 0; index < 10; ++index ) {
		history.push_back( tool_result_message( 8000 ) );
	}

	const auto plan = plan_context( history, occupancy( 9000, 10000 ), 100 );

	REQUIRE( plan.action == context_action::clear );
}

TEST_CASE( "clearing keeps the newest results and the call records", "[context]" ) {
	auto history = std::vector< model::message >{ };

	for ( auto index = 0; index < 10; ++index ) {
		history.push_back( text_message( model::role::assistant, "call " + std::to_string( index ) ) );
		history.push_back( tool_result_message( 8000 ) );
	}

	const auto reclaimed = apply_clear( history, 2 );

	REQUIRE( reclaimed > 0 );

	// dropping the assistant messages would remove the model's path back to the payload.
	auto assistant_count = std::size_t{ 0 };
	auto cleared = std::size_t{ 0 };
	auto intact = std::size_t{ 0 };

	for ( const auto& message : history ) {
		for ( const auto& block : message.blocks ) {
			if ( block.kind == model::block_kind::text ) {
				++assistant_count;
			}

			if ( block.kind == model::block_kind::tool_result ) {
				if ( block.result_json.starts_with( CLEARED_RESULT_PREFIX ) ) {
					++cleared;
				} else {
					++intact;
				}
			}
		}
	}

	REQUIRE( assistant_count == 10 );
	REQUIRE( intact == 2 );
	REQUIRE( cleared == 8 );
}

TEST_CASE( "clearing more results than exist reclaims nothing", "[context]" ) {
	auto history = std::vector< model::message >{ };
	history.push_back( tool_result_message( 8000 ) );

	REQUIRE( apply_clear( history, 5 ) == 0 );
}

TEST_CASE( "the pinned state block is stable and states what matters", "[context]" ) {
	auto state = pinned_state{ };
	state.task = "add the trust gate";
	state.plan = "gate, then egress";
	state.modified_files = { "src/mcode/ext/loader.cxx" };
	state.open_errors = { "test_extension_trust failed" };

	const auto first = render_pinned_state( state );

	// Byte-stable across calls: the prefix has to be cacheable, so nothing in it
	// may depend on the clock or a live counter.
	REQUIRE( first == render_pinned_state( state ) );
	REQUIRE( first.find( "add the trust gate" ) != std::string::npos );
	REQUIRE( first.find( "src/mcode/ext/loader.cxx" ) != std::string::npos );
	REQUIRE( first.find( "test_extension_trust failed" ) != std::string::npos );
}

TEST_CASE( "an empty pinned state renders nothing", "[context]" ) {
	REQUIRE( render_pinned_state( pinned_state{ } ).empty( ) );
}
