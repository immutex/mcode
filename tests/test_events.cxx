#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "mcode/events/bus.hxx"

using namespace mcode;

namespace {

	auto make_event( const events::kind type, std::string payload = "{}" ) -> events::event {
		auto value = events::event{ };
		value.type = type;
		value.payload_json = std::move( payload );

		return value;
	}

}

TEST_CASE( "subscribers receive only the kinds they asked for", "[events]" ) {
	// Per-kind lists, not one global list with a filter: otherwise every
	// subscriber pays for every event type.
	auto bus = events::bus{ };
	auto deltas = std::vector< std::string >{ };
	auto calls = std::vector< std::string >{ };

	bus.subscribe( events::kind::assistant_delta, [&]( const events::event& value ) {
		deltas.push_back( value.payload_json );
	} );

	bus.subscribe( events::kind::tool_call, [&]( const events::event& value ) {
		calls.push_back( value.payload_json );
	} );

	bus.publish( make_event( events::kind::assistant_delta, "a" ) );
	bus.publish( make_event( events::kind::tool_call, "b" ) );
	bus.publish( make_event( events::kind::assistant_delta, "c" ) );
	bus.publish( make_event( events::kind::turn_end ) );

	REQUIRE( deltas.size( ) == 2 );
	REQUIRE( deltas[ 0 ] == "a" );
	REQUIRE( deltas[ 1 ] == "c" );

	REQUIRE( calls.size( ) == 1 );
	REQUIRE( calls[ 0 ] == "b" );
}

TEST_CASE( "handlers run in registration order", "[events]" ) {
	auto bus = events::bus{ };
	auto order = std::vector< int >{ };

	for ( auto index = 1; index <= 3; ++index ) {
		bus.subscribe( events::kind::turn_start, [&order, index]( const events::event& ) {
			order.push_back( index );
		} );
	}

	bus.publish( make_event( events::kind::turn_start ) );

	REQUIRE( order == std::vector< int >{ 1, 2, 3 } );
}

TEST_CASE( "the first veto wins and short-circuits", "[events]" ) {
	auto bus = events::bus{ };
	auto reached = std::vector< int >{ };

	bus.subscribe_veto( events::kind::tool_pre_call, [&]( const events::event& ) {
		reached.push_back( 1 );

		return std::optional< events::veto >{ };
	} );

	bus.subscribe_veto( events::kind::tool_pre_call, [&]( const events::event& ) {
		reached.push_back( 2 );

		return std::optional< events::veto >{ events::veto{
			.reason = "destructive command", .source = "policy" } };
	} );

	bus.subscribe_veto( events::kind::tool_pre_call, [&]( const events::event& ) {
		reached.push_back( 3 );

		return std::optional< events::veto >{ };
	} );

	const auto decision = bus.publish( make_event( events::kind::tool_pre_call ) );

	REQUIRE( decision.has_value( ) );
	REQUIRE( decision->reason == "destructive command" );

	// The third handler must not have run: first veto short-circuits.
	REQUIRE( reached == std::vector< int >{ 1, 2 } );
}

TEST_CASE( "a non-vetoable kind never returns a veto", "[events]" ) {
	// Only the Pre* kinds accept a veto, so a handler cannot accidentally block a
	// turn by returning a value.
	REQUIRE( events::is_vetoable( events::kind::tool_pre_call ) );
	REQUIRE( events::is_vetoable( events::kind::spawn_pre ) );
	REQUIRE( events::is_vetoable( events::kind::prompt_pre ) );

	REQUIRE_FALSE( events::is_vetoable( events::kind::tool_call ) );
	REQUIRE_FALSE( events::is_vetoable( events::kind::tool_result ) );
	REQUIRE_FALSE( events::is_vetoable( events::kind::turn_end ) );
	REQUIRE_FALSE( events::is_vetoable( events::kind::assistant_delta ) );
}

TEST_CASE( "a handler publishing during dispatch queues rather than recurses", "[events]" ) {
	// Flat, never recursive. The depth cap plus the pending deque makes infinite
	// recursion impossible by construction rather than by convention.
	auto bus = events::bus{ };
	auto observed = std::vector< std::string >{ };

	bus.subscribe( events::kind::turn_start, [&]( const events::event& ) {
		observed.push_back( "outer" );

		bus.publish( make_event( events::kind::turn_end, "nested" ) );
		observed.push_back( "outer-after-publish" );
	} );

	bus.subscribe( events::kind::turn_end, [&]( const events::event& value ) {
		observed.push_back( "inner:" + value.payload_json );
	} );

	bus.publish( make_event( events::kind::turn_start ) );

	// The nested event is delivered AFTER the outer handler returns, not inside it.
	REQUIRE( observed.size( ) == 3 );
	REQUIRE( observed[ 0 ] == "outer" );
	REQUIRE( observed[ 1 ] == "outer-after-publish" );
	REQUIRE( observed[ 2 ] == "inner:nested" );

	// Depth never exceeded one level of real recursion.
	REQUIRE( bus.max_depth( ) == 1 );
	REQUIRE( bus.pending_count( ) == 0 );
}

TEST_CASE( "an event loop between two handlers terminates", "[events]" ) {
	// A handler that publishes its own event is the case that hangs a naive bus.
	auto bus = events::bus{ };
	auto deliveries = 0;

	bus.subscribe( events::kind::step_start, [&]( const events::event& ) {
		++deliveries;

		if ( deliveries < 5 ) {
			bus.publish( make_event( events::kind::step_start ) );
		}
	} );

	bus.publish( make_event( events::kind::step_start ) );

	REQUIRE( deliveries == 5 );
	REQUIRE( bus.max_depth( ) == 1 );
	REQUIRE( bus.pending_count( ) == 0 );
}

TEST_CASE( "unsubscribing mid-dispatch does not invalidate iteration", "[events]" ) {
	auto bus = events::bus{ };
	auto reached = std::vector< int >{ };
	auto first_id = events::bus::subscription_id{ 0 };

	first_id = bus.subscribe( events::kind::turn_start, [&]( const events::event& ) {
		reached.push_back( 1 );
	} );

	bus.subscribe( events::kind::turn_start, [&]( const events::event& ) {
		reached.push_back( 2 );

		// Removing a subscription while the list is being walked.
		bus.unsubscribe( first_id );
	} );

	bus.subscribe( events::kind::turn_start, [&]( const events::event& ) {
		reached.push_back( 3 );
	} );

	bus.publish( make_event( events::kind::turn_start ) );

	// All three ran this dispatch; the removal takes effect for the next one.
	REQUIRE( reached == std::vector< int >{ 1, 2, 3 } );
	REQUIRE( bus.subscriber_count( events::kind::turn_start ) == 2 );

	reached.clear( );
	bus.publish( make_event( events::kind::turn_start ) );

	REQUIRE( reached == std::vector< int >{ 2, 3 } );
}

TEST_CASE( "a throwing handler is contained and counted, not removed", "[events]" ) {
	// Dispatch is noexcept at the bus boundary. Auto-removing a throwing handler
	// would make a transient bug permanent.
	auto bus = events::bus{ };
	auto after_threw = false;

	bus.subscribe( events::kind::turn_start, []( const events::event& ) {
		throw std::runtime_error{ "handler bug" };
	} );

	bus.subscribe( events::kind::turn_start, [&]( const events::event& ) {
		after_threw = true;
	} );

	REQUIRE_NOTHROW( bus.publish( make_event( events::kind::turn_start ) ) );

	// The peer handler still ran.
	REQUIRE( after_threw );
	REQUIRE( bus.handler_failures( ) == 1 );

	// And the handler is still subscribed.
	REQUIRE( bus.subscriber_count( events::kind::turn_start ) == 2 );

	bus.publish( make_event( events::kind::turn_start ) );
	REQUIRE( bus.handler_failures( ) == 2 );
}

TEST_CASE( "kind tags are stable and complete", "[events]" ) {
	// The numeric value is the log's primary key, so a renumbering would corrupt
	// every existing session file.
	REQUIRE( static_cast< std::uint16_t >( events::kind::session_start ) == 0 );
	REQUIRE( static_cast< std::uint16_t >( events::kind::tool_pre_call ) == 7 );
	REQUIRE( static_cast< std::uint16_t >( events::kind::error ) == 15 );

	REQUIRE( events::to_string( events::kind::tool_pre_call ) == "tool.pre_call" );
	REQUIRE( events::to_string( events::kind::assistant_delta ) == "assistant.delta" );

	// Every kind has a distinct, non-placeholder name.
	auto names = std::vector< std::string_view >{ };

	for ( auto value = std::uint16_t{ 0 }; value < events::KIND_COUNT; ++value ) {
		names.push_back( events::to_string( static_cast< events::kind >( value ) ) );
	}

	for ( const auto& name : names ) {
		REQUIRE( name != "unknown" );
		REQUIRE( name.find( '.' ) != std::string_view::npos );
	}

	REQUIRE( events::KIND_COUNT == names.size( ) );
}
