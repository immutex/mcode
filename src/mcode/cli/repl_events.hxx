#pragma once

#include <atomic>
#include <vector>

#include "mcode/events/bus.hxx"
#include "mcode/tui/render.hxx"

// The TUI REPL's bridge from the loop's event bus to the render queue: one
// subscription per event kind the transcript shows. The ids are returned so
// the caller owns them for as long as the loop lives.
[[nodiscard]] auto subscribe_event_feed( mcode::events::bus& bus, mcode::tui::event_queue& queue,
	const std::atomic< bool >& interrupted )
	-> std::vector< mcode::events::bus::subscription_id >;
