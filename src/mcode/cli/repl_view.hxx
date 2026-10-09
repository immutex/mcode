#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "mcode/agent/loop.hxx"
#include "mcode/tui/editor.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"

// The prompt loop's view and key glue: the small helpers `run_repl` uses to
// keep the coordinator fed and to map terminal keys onto the view and the
// editor. Everything here is called on the main thread. `render_gate` is not a
// recursive mutex, so each function that touches the coordinator states
// whether it locks the gate or expects the caller to already hold it.

// Caller holds `render_gate`.
auto refresh( mcode::tui::render_coordinator& coordinator, const mcode::agent_loop& loop,
	const std::chrono::steady_clock::time_point& turn_started,
	const std::uint64_t& last_turn_elapsed_ms ) -> void;

[[nodiscard]] auto key_scroll_rows( const mcode::tui::tty_session& session_tty,
	const mcode::tui::key_event::kind type ) -> std::optional< int >;

// Locks `render_gate`.
auto scroll_view( mcode::tui::render_coordinator& coordinator, std::mutex& render_gate,
	const int rows ) -> void;

// Locks `render_gate`.
auto return_to_live( mcode::tui::render_coordinator& coordinator, std::mutex& render_gate ) -> void;

// Locks `render_gate`.
[[nodiscard]] auto view_scrolled( const mcode::tui::render_coordinator& coordinator,
	std::mutex& render_gate ) -> bool;

// Caller holds `render_gate`. Returns true when the key changed the view.
[[nodiscard]] auto handle_turn_key( mcode::tui::render_coordinator& coordinator,
	std::atomic< bool >& interrupted, const mcode::tui::tty_session& session_tty,
	const mcode::tui::key_event& key ) -> bool;

// The editor's own key event for a terminal key, or nothing when the key is
// not one the editor takes.
[[nodiscard]] auto translate_key( const mcode::tui::key_event& key )
	-> std::optional< mcode::tui::input_editor::key_event >;
