#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::tui {

	// One changed run: contiguous differing cells within one row.
	struct cell_run {
		std::size_t row = 0;
		std::size_t column = 0;
		std::size_t count = 0;
	};

	[[nodiscard]] auto diff_rows( const cell_buffer& previous, const cell_buffer& current )
		-> std::vector< cell_run >;

	// The ANSI emitter. Tracks the current pen, so an attribute change is
	// emitted only when it differs from what the terminal already shows. The
	// same buffer pair always produces the same bytes.
	class ansi_emitter {
	public:
		explicit ansi_emitter( const capabilities& caps );

		// Renders one frame's changed cells. The cursor is assumed PARKED on
		// the region's last row; every movement is relative to it, so the
		// region sits wherever the cursor is.
		[[nodiscard]] auto emit( const cell_buffer& previous, const cell_buffer& current )
			-> std::string;

		[[nodiscard]] auto synchronized( const std::string& frame ) const -> std::string;

		// Moves the parked cursor to one row and column of the live region.
		[[nodiscard]] auto caret( std::size_t parked_row, std::size_t row,
			std::size_t column ) const -> std::string;

		// Erases every region row and leaves the cursor parked on the last.
		[[nodiscard]] auto clear_region( std::size_t row_count ) const -> std::string;

		// Moves the parked cursor to the region's top row.
		[[nodiscard]] auto region_top( std::size_t row_count ) const -> std::string;

		// The bytes that move the pen to `value`. Committed scrollback is raw
		// text rather than a cell diff, so it resolves colour through here.
		[[nodiscard]] auto sgr( const style& value ) -> std::string;

	private:
		// A relative row move plus a column move, from a known row. Static: it
		// holds no pen state.
		[[nodiscard]] static auto move_to( std::size_t from_row, std::size_t to_row,
			std::size_t column ) -> std::string;

		capabilities caps_;
		style pen_;
		bool pen_valid_ = false;
	};

}
