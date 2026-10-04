#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcode::tui {

	// One editable line: content plus the caret's BYTE offset into it. The
	// editor owns no width table, so a column is never stored here.
	struct editor_line {
		std::string text;
		std::size_t cursor = 0;
	};

	// The multi-line input editor. It owns the buffer and the history and
	// knows nothing about the terminal.
	class input_editor {
	public:
		enum class key : std::uint8_t {
			character,
			enter,
			newline,
			backspace,
			delete_key,
			left,
			right,
			up,
			down,
			home,
			end,
			interrupt,
			escape,
		};

		struct key_event {
			key type = key::character;
			std::string text;
		};

		// Applies one key. Returns the completed submission when Enter closed
		// a non-empty buffer.
		//
		// enter continues the row on an odd backslash run before the caret, otherwise submits
		auto handle( const key_event& event ) -> std::optional< std::string >;

		// Clears the buffer for the next submission.
		auto reset( ) -> void;

		// Replaces the buffer with one line and parks the caret at its end.
		auto set_text( std::string text ) -> void;

		// The pending input as one line, rows joined by a space. The prompt row
		// is a single line, so this is what it echoes while typing.
		[[nodiscard]] auto text( ) const -> std::string;

		// The caret's byte offset across every row, which is what the one-line
		// prompt echo needs.
		[[nodiscard]] auto flattened_cursor( ) const noexcept -> std::size_t;

		// How many rows the buffer holds. One is the ordinary single-line
		// prompt; more means Enter continued the line.
		[[nodiscard]] auto row_count( ) const noexcept -> std::size_t {
			return lines_.size( );
		}

		// The submitted entries, oldest first. A read accessor only: the editor
		// ranks nothing, so a caller that wants a filtered view does the
		// ranking itself.
		[[nodiscard]] auto history( ) const noexcept -> const std::vector< std::string >& {
			return history_;
		}

		auto push_history( std::string entry ) -> void;

	private:
		std::vector< editor_line > lines_{ { } };
		std::size_t cursor_row_ = 0;
		std::vector< std::string > history_;
		std::optional< std::string > history_draft_;
		std::size_t history_position_ = 0;
	};

}
