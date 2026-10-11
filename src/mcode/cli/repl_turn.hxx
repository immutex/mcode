#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>

#include "mcode/cli/exec.hxx"
#include "mcode/cli/repl.hxx"
#include "mcode/agent/loop.hxx"
#include "mcode/tui/editor.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"

namespace mcode::cli::detail {

	// Everything the render and turn helpers touch. The interactive loop owns all of
	// it; the helpers are free functions over these references rather than lambdas
	// capturing twenty locals apiece.
	struct repl_runtime {
		mcode::tui::tty_session* tty = nullptr;
		mcode::tui::render_coordinator* coordinator = nullptr;
		mcode::tui::input_editor* editor = nullptr;
		mcode::tui::event_queue* queue = nullptr;
		mcode::agent_loop* loop = nullptr;
		mcode::cli::session* turn = nullptr;
		std::mutex* render_gate = nullptr;
		std::atomic< bool >* turn_done = nullptr;
		std::atomic< bool >* approval_active = nullptr;
		std::atomic< bool >* interrupted = nullptr;
		std::thread* worker = nullptr;
		mcode::cli::exit_code* last_code = nullptr;
		std::chrono::steady_clock::time_point* turn_started = nullptr;
		std::uint64_t* last_turn_elapsed_ms = nullptr;
	};

	// Both require the caller to hold `render_gate`.
	auto repaint( const repl_runtime& runtime ) -> void;
	auto show_prompt( const repl_runtime& runtime ) -> void;

	// Blocks until the turn thread finishes, repainting from the event queue meanwhile.
	auto run_one_turn( const repl_runtime& runtime, std::string_view submitted_text,
		std::string_view prompt_text ) -> void;

}
