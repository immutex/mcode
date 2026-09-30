#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/tty.hxx"
#include <cstddef>

namespace mcode::tui {

	// One changed run: cells in one row that differ, contiguous. The row is
	// carried so the emitter never has to re-derive it.
	struct cell_run {
		std::size_t row = 0;
		std::size_t column = 0;
		std::size_t count = 0;
	};

	// Row-granular first, then cell-level within changed rows. A row with no
	// change produces nothing.
	[[nodiscard]] auto diff_rows( const cell_buffer& previous, const cell_buffer& current )
		-> std::vector< cell_run >;

	// The ANSI emitter. Tracks the current pen so an attribute change is
	// emitted only when it differs from what the terminal already shows.
	//
	// The emitter is deterministic: the same buffer pair always produces the
	// same bytes, which is what makes the CI assertion possible.
	class ansi_emitter {
	public:
		explicit ansi_emitter( const capabilities& caps );

		// Renders one frame: cursor to the region top, changed runs, cursor
		// back to the input row. Empty when nothing changed.
		[[nodiscard]] auto emit( const cell_buffer& previous, const cell_buffer& current,
			std::size_t region_row ) -> std::string;

		// Wraps a frame in synchronized-output markers when supported.
		[[nodiscard]] auto synchronized( const std::string& frame ) const -> std::string;

		// Clears the live region and returns the cursor to the input row.
		[[nodiscard]] auto clear_region( std::size_t region_row,
			std::size_t row_count ) const -> std::string;

	private:
		[[nodiscard]] auto sgr( const style& value ) -> std::string;

		capabilities caps_;
		style pen_;
		bool pen_valid_ = false;
	};

	// Resolves a theme token to its escape fragment at the session's depth.
	// One resolution per token per session; the depth is never per cell.
	[[nodiscard]] auto token_color( token value, capabilities::color_depth depth )
		-> std::string_view;

}
