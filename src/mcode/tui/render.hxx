#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/markdown.hxx"
#include "mcode/tui/palette.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::tui {

	inline constexpr std::size_t LIVE_REGION_MIN_ROWS = 2;

	// Ceiling, so a runaway list cannot push the transcript off the screen.
	inline constexpr std::size_t LIVE_REGION_MAX_ROWS = 16;

	inline constexpr std::size_t LIVE_THOUGHT_MAX_ROWS = 6;

	inline constexpr std::size_t SCROLLBACK_MAX_ROWS = 2000;

	inline constexpr std::string_view HISTORY_INDICATOR = "── history (End to return) ──";

	inline constexpr std::string_view ACTIVITY_HINT = "esc to interrupt";

	inline constexpr std::uint64_t CONTEXT_WARN_PERCENT = 80;
	inline constexpr std::uint64_t CONTEXT_ERROR_PERCENT = 95;

	inline constexpr std::string_view USER_GUTTER = "❯";

	inline constexpr std::string_view PROMPT_PREFIX = "> ";
	inline constexpr std::size_t PROMPT_PREFIX_WIDTH = PROMPT_PREFIX.size( );

	struct render_state {
		struct active_tool {
			std::string verb;
			std::string target;
			std::uint64_t started_ms = 0;
			std::uint64_t elapsed_ms = 0;
			std::size_t spinner_frame = 0;
		};

		std::vector< active_tool > tools;

		// `rows` is materialised from `text` on read, which is why the pair is mutable.
		std::string thinking_text;
		mutable std::vector< styled_line > thinking_rows;

		std::string streaming_text;
		mutable std::vector< styled_line > streaming_rows;

		mutable bool thinking_stale = false;
		mutable bool streaming_stale = false;

		mutable std::uint64_t stream_render_count = 0;

		std::vector< styled_line > pending_commit;

		std::deque< styled_line > scrollback;

		// Zero is the live view; a commit advances it by the count it appends.
		std::size_t scroll_offset = 0;

		slash_palette palette;

		std::string model_name;
		std::uint64_t total_tokens = 0;
		double total_cost = 0.0;
		std::uint64_t turn_elapsed_ms = 0;

		// Capacity zero means unknown, and the percentage is then left out.
		std::uint64_t context_used = 0;
		std::uint64_t context_capacity = 0;

		std::string activity;

		std::string input_line;
		std::size_t input_cursor = 0;
	};

	[[nodiscard]] auto build_frame( const render_state& state, std::size_t row_count,
		std::size_t column_count, std::size_t ambiguous_width ) -> cell_buffer;

	[[nodiscard]] auto region_rows_for( const render_state& state, std::size_t screen_rows )
		-> std::size_t;

	class event_queue {
	public:
		enum class kind : std::uint8_t {
			assistant_delta,
			thinking_delta,
			tool_start,
			tool_end,
			turn_start,
			turn_end,
		};

		struct item {
			kind type = kind::assistant_delta;
			std::string text;
			std::string target;
			std::uint64_t stamp_ms = 0;
		};

		auto push( item value ) -> void;
		[[nodiscard]] auto drain( ) -> std::vector< item >;
		[[nodiscard]] auto empty( ) const noexcept -> bool;

	private:
		mutable std::mutex guard_;
		std::deque< item > items_;
	};

	class render_coordinator {
	public:
		render_coordinator( );

		auto set_capabilities( const capabilities& value ) -> void;

		auto resize( std::size_t screen_rows, std::size_t screen_columns ) -> void;

		[[nodiscard]] auto park( ) const -> std::string;

		// For a caller erasing after the last flush: the height is dynamic.
		[[nodiscard]] auto painted_height( ) const noexcept -> std::size_t {
			return painted_rows_;
		}

		auto invalidate( ) -> void;

		auto apply( const event_queue::item& value ) -> void;

		auto advance_tools( std::uint64_t now_ms ) -> void;

		[[nodiscard]] auto flush( ) -> std::string;

		[[nodiscard]] auto commit( std::vector< styled_line > lines ) -> std::string;

		auto queue_block( std::vector< styled_line > lines ) -> void;

		auto queue_rows( std::vector< styled_line > lines ) -> void;

		auto queue_text( std::string_view text, token color = token::text ) -> void;

		[[nodiscard]] auto state( ) const -> const render_state&;

		[[nodiscard]] auto stream_render_count( ) const noexcept -> std::uint64_t {
			return state_.stream_render_count;
		}

		// `cursor_byte` is a byte offset into `text`, not a display column.
		auto set_prompt( std::string text, std::size_t cursor_byte ) -> void;

		auto set_palette( slash_palette value ) -> void;

		auto set_meter( std::string model_name, std::uint64_t tokens, double cost,
			std::uint64_t elapsed_ms ) -> void;

		auto set_context( std::uint64_t used, std::uint64_t capacity ) -> void;

		auto set_activity( std::string_view verb ) -> void;

		auto scroll_by( int rows ) -> void;

		auto scroll_to_bottom( ) -> void;

		[[nodiscard]] auto scrolled( ) const noexcept -> bool {
			return state_.scroll_offset > 0;
		}

	private:
		auto retain( const std::vector< styled_line >& rows ) -> void;
		auto clamp_scroll( ) -> void;
		auto queue_thought( ) -> void;

		[[nodiscard]] auto region_rows( ) const -> std::size_t;

		capabilities caps_{ };
		render_state state_;
		cell_buffer previous_;
		cell_buffer current_;
		std::size_t screen_rows_ = 24;
		std::size_t screen_columns_ = 80;

		std::size_t painted_rows_ = LIVE_REGION_MIN_ROWS;

		bool transcript_started_ = false;

		bool resize_pending_ = false;

		bool repaint_all_ = false;
	};

	// Braille, one frame per SPINNER_INTERVAL_MS. Every glyph is single-width.
	[[nodiscard]] auto spinner_glyph( std::size_t frame ) -> std::string_view;

	inline constexpr std::size_t SPINNER_FRAMES = 10;
	inline constexpr std::uint64_t SPINNER_INTERVAL_MS = 100;

}
