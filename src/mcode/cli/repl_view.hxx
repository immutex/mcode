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

// `render_gate` is not recursive, so each function states whether it locks it.

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

[[nodiscard]] auto translate_key( const mcode::tui::key_event& key )
	-> std::optional< mcode::tui::input_editor::key_event >;
