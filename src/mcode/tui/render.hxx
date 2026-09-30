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

		// The last open markdown block's rendered lines; closed blocks were
		// already committed to scrollback.
		styled_line streaming_line;

		// The committed-tool lines pending display, newest last.
		std::vector< styled_line > committed;

		// The status line: model, tokens, cost, elapsed.
		std::string model_name;
		std::uint64_t total_tokens = 0;
		double total_cost = 0.0;
		std::uint64_t turn_elapsed_ms = 0;

		// The prompt row content the editor renders.
		std::string input_line;
		std::size_t input_cursor = 0;
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
			usage,
			shutdown,
		};

		struct item {
			kind type = kind::assistant_delta;
			std::string text;
			std::uint64_t tokens = 0;
			double cost = 0.0;
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

		auto resize( std::size_t row_count, std::size_t column_count ) -> void;

		// Applies one queue item to the state.
		auto apply( const event_queue::item& value ) -> void;

		// Builds the current frame and returns the bytes to write. Empty when
		// nothing changed since the last call.
		[[nodiscard]] auto flush( ) -> std::string;

		// Commits the streaming line to scrollback and clears the live
		// region. Returns the bytes to write.
		[[nodiscard]] auto commit_streaming( ) -> std::string;

		// Commits one finished tool line.
		[[nodiscard]] auto commit_tool( const styled_span& line ) -> std::string;

		[[nodiscard]] auto state( ) const noexcept -> const render_state& { return state_; }

	private:
		capabilities caps_{ };
		render_state state_;
		cell_buffer previous_;
		cell_buffer current_;
		bool first_frame_ = true;
	};

	// The spinner glyphs: braille, 80 ms per frame, skipped when idle. Every
	// glyph is single-width with no variation selector.
	[[nodiscard]] auto spinner_glyph( std::size_t frame ) -> std::string_view;

	inline constexpr std::size_t SPINNER_FRAMES = 10;

	// The live region's row count: prompt + status + streaming + tool rows.
	inline constexpr std::size_t LIVE_REGION_ROWS = 6;

}
