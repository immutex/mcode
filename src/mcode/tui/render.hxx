#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/tui/cell.hxx"
#include "mcode/tui/tty.hxx"
#include "mcode/tui/diff_view.hxx"
#include "mcode/tui/markdown.hxx"
#include <cstddef>
#include <cstdint>

namespace mcode::tui {

	// What the live region shows. One row per active tool call, the streaming
	// markdown tail, and the status line. This is the frame builder's whole
	// input: a pure function from here to a cell buffer.
	struct render_state {
		// One active tool call per row: spinner glyph, verb, target, elapsed.
		struct active_tool {
			std::string verb;
			std::string target;
			std::uint64_t elapsed_ms = 0;
			std::size_t spinner_frame = 0;
		};

		std::vector< active_tool > tools;

		// The answer as it streams, and its rendered form. The text is kept
		// because each delta is a fragment: assigning the render of one delta
		// over the last showed only the newest chunk, so a long answer flickered
		// through single fragments instead of growing.
		std::string streaming_text;
		styled_line streaming_line;

		// An answer finished but not yet written to scrollback. A turn's text
		// arrives as deltas into the live region, and the region is cleared
		// when the turn ends -- so without this the answer was erased before
		// the user could read it. `flush` commits it on the next paint.
		std::string pending_commit;

		// The status line: model, tokens, cost, elapsed.
		std::string model_name;
		std::uint64_t total_tokens = 0;
		double total_cost = 0.0;
		std::uint64_t turn_elapsed_ms = 0;

		// The prompt row content the editor renders, and the caret's column
		// within it, in display columns. The caret is placed by the emitter
		// from this; nothing else knows where the prompt row is.
		std::string input_line;
		std::size_t input_cursor = 0;

		// The prompt's column on screen, so the caret can be addressed in
		// absolute coordinates.
		std::size_t input_column = 0;
	};

	// The pure builder: state in, cells out. No I/O, no clock, no terminal.
	// Deterministic by construction, which is what the twice-identical-bytes
	// acceptance check exercises.
	[[nodiscard]] auto build_frame( const render_state& state, std::size_t row_count,
		std::size_t column_count, std::size_t ambiguous_width ) -> cell_buffer;

	// The event queue. Producers push copies; the coordinator drains. The
	// bus stays single-threaded -- its handlers run on the loop thread and
	// push into here, so the renderer never touches the bus. The queue is
	// the one crossing point between the loop thread and the render thread,
	// so push and drain share a mutex.
	class event_queue {
	public:
		enum class kind : std::uint8_t {
			assistant_delta,
			tool_start,
			tool_end,
			turn_start,
			turn_end,
		};

		struct item {
			kind type = kind::assistant_delta;
			std::string text;
		};

		auto push( item value ) -> void;
		[[nodiscard]] auto drain( ) -> std::vector< item >;
		[[nodiscard]] auto empty( ) const noexcept -> bool;

	private:
		mutable std::mutex guard_;
		std::deque< item > items_;
	};

	// The commit protocol: when a block completes, the coordinator clears the
	// live region, prints the finalized lines to scrollback with a trailing
	// newline, and redraws. Committed lines are never repainted.
	class render_coordinator {
	public:
		render_coordinator( );

		// The session's depth, probed once. NO_COLOR lands here as
		// color_depth::none and every token resolves to attributes only.
		auto set_capabilities( const capabilities& value ) -> void;
		[[nodiscard]] auto caps( ) const noexcept -> const capabilities& { return caps_; }

		// The SCREEN's geometry, not the live region's. The region is the
		// bottom `LIVE_REGION_ROWS` of the screen: committed transcript lines
		// live in scrollback above it, and a region anchored at row 0 would
		// repaint over the top of that scrollback on every frame.
		auto resize( std::size_t screen_rows, std::size_t screen_columns ) -> void;

		// Parks the cursor on the screen's last row, which is the region's last
		// row. Emitted once at startup, before the first frame.
		//
		// Absolute, and deliberately so: the region must sit at the BOTTOM of
		// the screen, above nothing but the transcript in scrollback. Counting
		// newlines to get there left it at the top, where every frame painted
		// over the top of the scrollback instead of below it.
		[[nodiscard]] auto reserve( ) const -> std::string;

		// Moves the cursor to the screen's last row -- the region's last row.
		//
		// Absolute, unlike every other movement: it re-establishes the parked
		// position after something wrote to the console and left the cursor
		// elsewhere. The relative moves assume the parked row, so a cursor left
		// one row off made the next clear start in the wrong place and the
		// previous prompt stayed on screen.
		[[nodiscard]] auto park( ) const -> std::string;

		// Forgets what is on screen, so the next flush repaints every row.
		//
		// Anything that writes to the console outside `flush` -- the approval
		// prompt -- makes the tracked previous frame a lie, and a diff against
		// a lie leaves the region half-painted.
		auto invalidate( ) -> void;

		// Applies one queue item to the state.
		auto apply( const event_queue::item& value ) -> void;

		// Builds the current frame and returns the bytes to write. Empty when
		// nothing changed since the last call.
		[[nodiscard]] auto flush( ) -> std::string;

		// Writes finished text to scrollback and re-reserves the region.
		[[nodiscard]] auto commit( std::string text ) -> std::string;

		[[nodiscard]] auto state( ) const noexcept -> const render_state& { return state_; }

		// The prompt row's content. Set directly rather than through the
		// queue: it is local session state, not an event, and the first frame
		// needs it before any event exists.
		// `cursor_byte` is the caret's offset within `text` in BYTES, which is
		// what the editor tracks. The conversion to a display column happens
		// here, because the width table is this layer's business.
		auto set_prompt( std::string text, std::size_t cursor_byte ) -> void;

		// The status line's meter: model, tokens, cost, elapsed. Pushed from
		// the loop's budget rather than derived from events, because the budget
		// is what the run is actually charged against.
		auto set_meter( std::string model_name, std::uint64_t tokens, double cost,
			std::uint64_t elapsed_ms ) -> void;

	private:
		capabilities caps_{ };
		render_state state_;
		cell_buffer previous_;
		cell_buffer current_;

		// The screen's height, for parking the cursor on its last row. The
		// region is the bottom of the screen, so its position depends on this
		// and cannot be derived from the buffers, which are only the region.
		std::size_t screen_rows_ = 24;

	};

	// The spinner glyphs: braille, 80 ms per frame, skipped when idle. Every
	// glyph is single-width with no variation selector.
	[[nodiscard]] auto spinner_glyph( std::size_t frame ) -> std::string_view;

	inline constexpr std::size_t SPINNER_FRAMES = 10;

	// The prompt's marker. Its width is the caret's base column, so the two are
	// declared together rather than measured twice.
	inline constexpr std::string_view PROMPT_PREFIX = "> ";
	inline constexpr std::size_t PROMPT_PREFIX_WIDTH = PROMPT_PREFIX.size( );

	// The live region's row count: prompt + status + streaming + tool rows.
	inline constexpr std::size_t LIVE_REGION_ROWS = 6;

}
