#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcode::tui {

	// One editable line: content plus the caret's BYTE offset into it. The
	// editor owns no width table, so a column is never stored here.
	struct editor_line {
		std::string text;
		std::size_t cursor = 0;
	};

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

		// Returns the completed submission when Enter closed a non-empty buffer.
		// An odd run of backslashes continues the row instead of submitting.
		auto handle( const key_event& event ) -> std::optional< std::string >;

		auto reset( ) -> void;

		auto set_text( std::string text ) -> void;

		// Never submits: a pasted block is content, and its line breaks are rows.
		auto insert_text( std::string_view text ) -> void;

		[[nodiscard]] auto text( ) const -> std::string;

		[[nodiscard]] auto flattened_cursor( ) const noexcept -> std::size_t;

		[[nodiscard]] auto row_count( ) const noexcept -> std::size_t {
			return lines_.size( );
		}

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
