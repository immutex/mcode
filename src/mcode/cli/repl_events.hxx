#pragma once

#include <atomic>
#include <vector>

#include "mcode/events/bus.hxx"
#include "mcode/tui/render.hxx"

// Returned so the caller keeps the subscriptions alive for the loop's lifetime.
[[nodiscard]] auto subscribe_event_feed( mcode::events::bus& bus, mcode::tui::event_queue& queue,
	const std::atomic< bool >& interrupted )
	-> std::vector< mcode::events::bus::subscription_id >;
