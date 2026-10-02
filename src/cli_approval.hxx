#pragma once

#include <atomic>
#include <memory>

#include "mcode/tui/approval_tui.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::cli {

	// `active` is set while the question owns the console, so the repaint pump holds off.
	[[nodiscard]] auto make_approval_source( mcode::tui::tty_session& session,
		std::atomic< bool >& active, mcode::tui::render_coordinator& coordinator )
		-> mcode::tui::ui_approval_source;

}
