#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include <cstddef>

namespace mcode::tui {

	// One rendered markdown block. `open` blocks can still grow; a closed
	// block is immutable and the renderer commits it.
	struct md_block {
		enum class kind : std::uint8_t {
			paragraph,
			heading,
			fenced_code,
			list_item,
		};

		kind type = kind::paragraph;

		// For heading: 1-6. For fenced_code: the info string, empty when absent.
		std::size_t level = 0;
		std::string info;

		std::vector< styled_line > lines;
		bool open = true;
	};

	// Parses one incremental chunk into block boundaries. The parser is
	// chunk-oriented: a closed block never re-renders, only the last open
	// block absorbs the next chunk.
	class markdown_parser {
	public:
		// Appends streamed text and returns the blocks that changed since the
		// last call. The last block in the returned range is the open one.
		auto feed( std::string_view chunk ) -> std::vector< md_block >;

		// Closes the trailing open block, if any. Called when the stream
		// ends.
		auto finish( ) -> std::vector< md_block >;

		[[nodiscard]] auto blocks( ) const noexcept -> const std::vector< md_block >& {
			return blocks_;
		}

	private:
		auto close_open( ) -> void;
		auto start_block( md_block::kind type, std::size_t level, std::string_view info )
			-> void;

		std::vector< md_block > blocks_;
		std::string carry_;
		bool stream_done_ = false;
	};

	// Inline formatting: bold, italic, inline code. One pass, no nesting
	// beyond one level of emphasis around code.
	[[nodiscard]] auto render_inline( std::string_view text, token base ) -> styled_line;

}
