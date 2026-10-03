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

	// The region's floor: prompt + status. The palette and tool rows add to it,
	// so the height is dynamic.
	inline constexpr std::size_t LIVE_REGION_ROWS = 6;

	// Ceiling, so a runaway list cannot push the transcript off the screen.
	inline constexpr std::size_t LIVE_REGION_MAX_ROWS = 16;

	// What the user's own message is prefixed with once submitted.
	inline constexpr std::string_view USER_GUTTER = "❯";

	// The caret's base column is this marker's width.
	inline constexpr std::string_view PROMPT_PREFIX = "> ";
	inline constexpr std::size_t PROMPT_PREFIX_WIDTH = PROMPT_PREFIX.size( );

	// The frame builder's whole input.
	struct render_state {
		struct active_tool {
			std::string verb;
			std::string target;
			std::uint64_t started_ms = 0;
			std::uint64_t elapsed_ms = 0;
			std::size_t spinner_frame = 0;
		};

		std::vector< active_tool > tools;

		// The model's reasoning, streamed before its answer. Rendered in its
		// own colour and committed as a collapsed block. `rows` is the whole
		// block: the region shows all of it, bounded, not just the last line.
		//
		// A delta only appends to `text` and marks `rows` stale: re-rendering
		// the whole buffer per delta is quadratic in the answer's length. The
		// rows are materialised whole by the next reader instead, so a burst
		// of deltas costs at most one render per paint.
		//
		// `rows` is a pure function of `text`, materialised on read, which is
		// why the pair is `mutable`.
		std::string thinking_text;
		mutable std::vector< styled_line > thinking_rows;

		// The answer, streamed the same way.
		std::string streaming_text;
		mutable std::vector< styled_line > streaming_rows;

		// Set by the delta path, cleared by the reader that re-renders. Every
		// path that hands out or commits the rows materialises them first, so
		// a reader never sees rows that lag the buffer. Mutable, because the
		// readers take the state by const reference.
		mutable bool thinking_stale = false;
		mutable bool streaming_stale = false;

		// Whole-buffer re-renders of the streamed blocks so far: the cost the
		// delta path used to pay per token, and no longer does.
		mutable std::uint64_t stream_render_count = 0;

		// Finished blocks waiting to be written to scrollback, styled so the
		// commit can colour them. One entry is one committed block, which may
		// span several rows; an empty entry is the blank line between groups.
		std::vector< styled_line > pending_commit;

		slash_palette palette;

		std::string model_name;
		std::uint64_t total_tokens = 0;
		double total_cost = 0.0;
		std::uint64_t turn_elapsed_ms = 0;

		std::string input_line;
		std::size_t input_cursor = 0;
	};

	// The frame builder's whole input. Reads the streamed rows, so it brings
	// them up to date first: a frame can never paint a stale block.
	[[nodiscard]] auto build_frame( const render_state& state, std::size_t row_count,
		std::size_t column_count, std::size_t ambiguous_width ) -> cell_buffer;

	// How many rows the state needs, bounded by the terminal. Reads the
	// streamed rows, so it brings them up to date first: a region is never
	// sized from stale rows.
	[[nodiscard]] auto region_rows_for( const render_state& state, std::size_t screen_rows )
		-> std::size_t;

	// The event queue: the one crossing point between the loop thread, which
	// pushes, and the render thread, which drains.
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

			// The tool call's subject, for the live row.
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

	// Owns the live region and the commit protocol. The region is the bottom
	// rows of the screen; finished output is written to the terminal's own
	// scrollback above it and never repainted.
	class render_coordinator {
	public:
		render_coordinator( );

		auto set_capabilities( const capabilities& value ) -> void;

		// The SCREEN's geometry. The region's height is derived per frame.
		auto resize( std::size_t screen_rows, std::size_t screen_columns ) -> void;

		// Emitted once at startup: scrolls the region into existence.
		[[nodiscard]] auto reserve( ) const -> std::string;

		// Puts the cursor back on the region's last row. Every other movement
		// is relative to it.
		[[nodiscard]] auto park( ) const -> std::string;

		// Forgets the tracked frame, so the next flush repaints every row.
		// Anything writing to the console outside `flush` must call this.
		auto invalidate( ) -> void;

		auto apply( const event_queue::item& value ) -> void;

		// Animates a running call.
		auto advance_tools( std::uint64_t now_ms ) -> void;

		// The bytes to write for the current state. Empty when nothing changed.
		[[nodiscard]] auto flush( ) -> std::string;

		// Writes finished lines to scrollback and re-reserves the region.
		[[nodiscard]] auto commit( std::vector< styled_line > lines ) -> std::string;

		// Queues a message block: one blank line separates it from the block
		// before it, so groups never run together.
		auto queue_block( std::vector< styled_line > lines ) -> void;

		// Queues rows that continue the group already open: the tool rows of
		// one step, and the thought line they follow, stay adjacent.
		auto queue_rows( std::vector< styled_line > lines ) -> void;

		// Splits plain text on newlines and queues it as one message block.
		auto queue_text( std::string_view text, token color = token::text ) -> void;

		// The frame builder's input, with the streamed blocks materialised:
		// reading the row vectors can never disagree with the buffer they were
		// rendered from. Materialising allocates, so this is not `noexcept`.
		[[nodiscard]] auto state( ) const -> const render_state&;

		// Whole-buffer re-renders of the streamed blocks so far. A burst of
		// deltas costs one per paint, not one per delta, and this is what
		// makes that bound observable. It reports the work already done, so
		// unlike `state` it never renders anything itself.
		[[nodiscard]] auto stream_render_count( ) const noexcept -> std::uint64_t {
			return state_.stream_render_count;
		}

		// The prompt row's content. `cursor_byte` is a byte offset into `text`.
		auto set_prompt( std::string text, std::size_t cursor_byte ) -> void;

		// Replaces the palette. The caller filters; the coordinator renders.
		auto set_palette( slash_palette value ) -> void;

		auto set_meter( std::string model_name, std::uint64_t tokens, double cost,
			std::uint64_t elapsed_ms ) -> void;

	private:
		// Commits the reasoning as one bounded block, if any is pending.
		auto queue_thought( ) -> void;

		[[nodiscard]] auto region_rows( ) const -> std::size_t;

		capabilities caps_{ };
		render_state state_;
		cell_buffer previous_;
		cell_buffer current_;
		std::size_t screen_rows_ = 24;
		std::size_t screen_columns_ = 80;
		std::size_t painted_rows_ = LIVE_REGION_ROWS;

		// True once any row has been queued, so the first group is not preceded
		// by a blank line.
		bool transcript_started_ = false;
	};

	// Braille, one frame per SPINNER_INTERVAL_MS. Every glyph is single-width.
	[[nodiscard]] auto spinner_glyph( std::size_t frame ) -> std::string_view;

	inline constexpr std::size_t SPINNER_FRAMES = 10;
	inline constexpr std::uint64_t SPINNER_INTERVAL_MS = 100;

}
