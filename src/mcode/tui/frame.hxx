#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::tui {

	struct cell_run {
		std::size_t row = 0;
		std::size_t column = 0;
		std::size_t count = 0;
	};

	[[nodiscard]] auto diff_rows( const cell_buffer& previous, const cell_buffer& current )
		-> std::vector< cell_run >;

	// Tracks the current pen, so an attribute change is emitted only when it
	// differs from what the terminal already shows.
	class ansi_emitter {
	public:
		explicit ansi_emitter( const capabilities& caps );

		// renders changed cells from the parked row; `full` emits every column of every row
		[[nodiscard]] auto emit( const cell_buffer& previous, const cell_buffer& current,
			bool full = false ) -> std::string;

		[[nodiscard]] auto synchronized( const std::string& frame ) const -> std::string;

		[[nodiscard]] auto caret( std::size_t parked_row, std::size_t row,
			std::size_t column ) const -> std::string;

		[[nodiscard]] auto clear_region( std::size_t row_count ) const -> std::string;

		[[nodiscard]] auto region_top( std::size_t row_count ) const -> std::string;

		// Committed scrollback is raw text rather than a cell diff, so it
		// resolves colour through here.
		[[nodiscard]] auto sgr( const style& value ) -> std::string;

	private:
		[[nodiscard]] static auto move_to( std::size_t from_row, std::size_t to_row,
			std::size_t column ) -> std::string;

		capabilities caps_;
		style pen_;
		bool pen_valid_ = false;
	};

}
