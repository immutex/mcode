#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "mcode/tui/approval_tui.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::cli {

	// `active` keeps the pump off the console; `gate` serializes every `render_coordinator` access.
	[[nodiscard]] auto make_approval_source( mcode::tui::tty_session& session,
		std::atomic< bool >& active, std::mutex& gate,
		mcode::tui::render_coordinator& coordinator )
		-> mcode::tui::ui_approval_source;

}
