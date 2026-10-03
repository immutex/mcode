#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/render.hxx"

namespace mcode::tui {

	// The row the history viewport's indicator occupies, so the retained rows
	// are grown to fill the rest of the region.
	inline constexpr std::size_t HISTORY_HEADER_ROWS = 1;

	// The region's height while the history viewport is up: the whole
	// available height, so a screenful of history is visible rather than the
	// live region's bounded few rows. Shared, because the scroll clamp has to
	// agree with the height the frame is built at.
	[[nodiscard]] auto history_region_rows( std::size_t screen_rows ) -> std::size_t;

	// One elapsed time, e.g. "1.2s". Shared, so a live row and the status line
	// format the same clock the same way.
	[[nodiscard]] auto format_elapsed( std::uint64_t ms ) -> std::string;

	// Writes the status line into the row below the cursor and moves the
	// cursor onto it. The context percentage and the activity verb live here,
	// so their thresholds, colours and hint have one home.
	auto write_status_line( cell_buffer& buffer, std::size_t& row, const render_state& state )
		-> void;

	// Fills the region from the retained scrollback: the indicator row at the
	// bottom, the newest retained rows above it. The viewport's whole body, so
	// the frame builder only chooses which view to draw.
	auto write_history_frame( cell_buffer& buffer, const render_state& state ) -> void;

}
