#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include <cstddef>
#include <cstdint>

namespace mcode::tui {

	// One editable line: content plus the cursor column, both in display
	// columns so a CJK character moves the cursor by two.
	struct editor_line {
		std::string text;
		std::size_t cursor = 0;
	};

	// The multi-line input editor. History, paste placeholder, ghost-text
	// autosuggest. Every mutation is a named operation the REPL drives; the
	// editor owns no terminal.
	class input_editor {
	public:
		// Keys the REPL translates from the raw input layer.
		enum class key : std::uint8_t {
			character,
			enter,
			shift_enter,
			backslash_newline,
			backspace,
			delete_key,
			left,
			right,
			up,
			down,
			home,
			end,
			paste,
			interrupt,
		};

		struct key_event {
			key type = key::character;
			std::string text;
		};

		// Applies one key. Returns the completed submission, when Enter
		// closed a non-empty buffer; empty otherwise.
		auto handle( const key_event& event ) -> std::optional< std::string >;

		// True when Ctrl+D arrived on an empty buffer: the session exits.
		[[nodiscard]] auto exit_requested( ) const noexcept -> bool { return exit_; }

		// The pending input, reset after each submit.
		auto reset( ) -> void;

		// The pending input as one line, its rows joined by a space. The
		// renderer's prompt row is a single line, so this is what it echoes
		// while typing; the submitted text is the rows themselves.
		[[nodiscard]] auto text( ) const -> std::string;

		[[nodiscard]] auto lines( ) const noexcept -> const std::vector< editor_line >& {
			return lines_;
		}

		[[nodiscard]] auto cursor_row( ) const noexcept -> std::size_t { return cursor_row_; }
		[[nodiscard]] auto cursor_column( ) const noexcept -> std::size_t {
			return lines_.empty( ) ? 0 : lines_[ cursor_row_ ].cursor;
		}

		// The ghost-text suggestion for the current last line, or empty.
		[[nodiscard]] auto suggestion( ) const -> std::string;

		// History navigation state.
		auto push_history( std::string entry ) -> void;
		[[nodiscard]] auto history( ) const noexcept -> const std::vector< std::string >& {
			return history_;
		}

		// Renders the pending input as one styled block: the prompt marker,
		// the lines, and the ghost suggestion in muted.
		[[nodiscard]] auto render( ) const -> std::vector< styled_line >;

	private:
		std::vector< editor_line > lines_{ { } };
		std::size_t cursor_row_ = 0;
		std::vector< std::string > history_;
		std::optional< std::string > history_draft_;
		std::size_t history_position_ = 0;
		bool exit_ = false;
	};

}
