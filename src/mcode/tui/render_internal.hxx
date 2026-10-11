#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/render.hxx"

namespace mcode::tui {

	inline constexpr std::size_t HISTORY_HEADER_ROWS = 1;

	[[nodiscard]] auto history_region_rows( std::size_t screen_rows ) -> std::size_t;

	// Shared, so a live row and the status line format the same clock alike.
	[[nodiscard]] auto format_elapsed( std::uint64_t ms ) -> std::string;

	auto write_status_line( cell_buffer& buffer, std::size_t& row, const render_state& state )
		-> void;

	auto write_history_frame( cell_buffer& buffer, const render_state& state ) -> void;

}
