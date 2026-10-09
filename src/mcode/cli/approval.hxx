#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "mcode/tui/approval_tui.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::cli {

	// `active` is set while the question owns the console, so the repaint pump
	// holds off. `gate` serializes every `render_coordinator` access: the
	// presenter draws on the worker thread while the pump repaints on the main
	// one, and both mutate the same live region.
	[[nodiscard]] auto make_approval_source( mcode::tui::tty_session& session,
		std::atomic< bool >& active, std::mutex& gate,
		mcode::tui::render_coordinator& coordinator )
		-> mcode::tui::ui_approval_source;

}
