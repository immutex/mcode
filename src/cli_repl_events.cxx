#include "cli_repl_events.hxx"

#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcode/events/bus.hxx"
#include "mcode/support/json.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"

namespace {

	[[nodiscard]] auto display_text( const mcode::events::kind type,
		const std::string& payload_json ) -> std::string {
		auto payload = mcode::json::document::parse( payload_json );

		if ( !payload ) {
			return { };
		}

		const auto field = [ & ]( const std::string_view pointer ) -> std::string {
			const auto found = payload->pointer_string( pointer );

			return found ? *found : std::string{ };
		};

		switch ( type ) {
			case mcode::events::kind::assistant_delta:
			case mcode::events::kind::assistant_thinking:
				return field( "/text" );
			case mcode::events::kind::tool_call:
			case mcode::events::kind::tool_result:
				return field( "/tool" );
			default:
				return { };
		}
	}

	[[nodiscard]] auto tool_target( const std::string& payload_json ) -> std::string {
		auto payload = mcode::json::document::parse( payload_json );

		if ( !payload ) {
			return { };
		}

		for ( const auto* pointer : { "/args/path", "/args/command" } ) {
			if ( const auto found = payload->pointer_string( pointer ) ) {
				return *found;
			}
		}

		return { };
	}

}

auto subscribe_event_feed( mcode::events::bus& bus, mcode::tui::event_queue& queue,
	const std::atomic< bool >& interrupted )
	-> std::vector< mcode::events::bus::subscription_id > {
	const auto feed = [ &queue, &interrupted ]( const mcode::tui::event_queue::kind target,
		const mcode::events::kind source ) {
		return [ &queue, &interrupted, target, source ](
			const mcode::events::event& value ) {
			// An interrupt stops consuming the stream: whatever arrived is
			// already in the transcript, and the remainder would only append
			// to text the user asked to stop.
			if ( interrupted.load( ) &&
				( source == mcode::events::kind::assistant_delta ||
					source == mcode::events::kind::assistant_thinking ) ) {
				return;
			}

			auto item = mcode::tui::event_queue::item{ };
			item.type = target;
			item.text = display_text( source, value.payload_json );
			item.stamp_ms = mcode::tui::monotonic_ms( );

			if ( source == mcode::events::kind::tool_call ) {
				item.target = tool_target( value.payload_json );
			}

			queue.push( std::move( item ) );
		};
	};

	auto subscriptions = std::vector< mcode::events::bus::subscription_id >{ };
	subscriptions.push_back( bus.subscribe(
		mcode::events::kind::assistant_delta, feed( mcode::tui::event_queue::kind::assistant_delta,
			mcode::events::kind::assistant_delta ) ) );
	subscriptions.push_back( bus.subscribe(
		mcode::events::kind::assistant_thinking,
		feed( mcode::tui::event_queue::kind::thinking_delta,
			mcode::events::kind::assistant_thinking ) ) );
	subscriptions.push_back( bus.subscribe(
		mcode::events::kind::tool_call, feed( mcode::tui::event_queue::kind::tool_start,
			mcode::events::kind::tool_call ) ) );
	subscriptions.push_back( bus.subscribe(
		mcode::events::kind::tool_result, feed( mcode::tui::event_queue::kind::tool_end,
			mcode::events::kind::tool_result ) ) );
	subscriptions.push_back( bus.subscribe(
		mcode::events::kind::turn_start, feed( mcode::tui::event_queue::kind::turn_start,
			mcode::events::kind::turn_start ) ) );
	subscriptions.push_back( bus.subscribe(
		mcode::events::kind::turn_end, feed( mcode::tui::event_queue::kind::turn_end,
			mcode::events::kind::turn_end ) ) );

	return subscriptions;
}
