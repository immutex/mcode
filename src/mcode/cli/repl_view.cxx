#include "mcode/cli/repl_view.hxx"

#include <chrono>
#include <mutex>
#include <string>

namespace {

	// One wheel notch. The terminal's own scrollback moves a few rows per
	// notch, so the in-app viewport does too.
	inline constexpr int WHEEL_SCROLL_ROWS = 3;

	[[nodiscard]] auto page_rows( const mcode::tui::tty_session& session_tty ) -> int {
		const auto measured = session_tty.size( ).second;

		return static_cast< int >( measured > 1 ? measured - 1 : 1 );
	}

	// The verb reflects what the turn is doing right now, read from the same
	// state the frame is built from. The renderer appends the ellipsis.
	//
	// The last case is the one that makes the harness feel alive: between
	// submitting and the first delta the model is thinking but nothing has
	// arrived, and every other signal is empty. Reporting nothing there left a
	// silent gap that reads as a hang, so the turn is asked whether it is still
	// running and the answer is `Waiting`.
	[[nodiscard]] auto activity_verb( const mcode::tui::render_coordinator& coordinator,
		const bool turn_running ) -> std::string {
		const auto& state = coordinator.state( );

		if ( !state.tools.empty( ) ) {
			return "Running " + state.tools.back( ).verb;
		}

		if ( !state.streaming_text.empty( ) ) {
			return "Working";
		}

		if ( !state.thinking_text.empty( ) ) {
			return "Thinking";
		}

		if ( turn_running ) {
			return "Waiting";
		}

		return { };
	}

}

auto refresh( mcode::tui::render_coordinator& coordinator, const mcode::agent_loop& loop,
	const std::chrono::steady_clock::time_point& turn_started,
	const std::uint64_t& last_turn_elapsed_ms ) -> void {
	const auto& budget = loop.budget( );
	const auto elapsed = turn_started == std::chrono::steady_clock::time_point{ }
		? last_turn_elapsed_ms
		: static_cast< std::uint64_t >( std::chrono::duration_cast<
			std::chrono::milliseconds >( std::chrono::steady_clock::now( )
				- turn_started ).count( ) );

	coordinator.set_meter( std::string{ loop.model_name( ) }, budget.tokens_used,
		budget.usd_used, elapsed );
	coordinator.set_context( loop.context_used( ), loop.context_capacity( ) );

	// A non-default start time is exactly "a turn is in flight": the REPL sets it
	// when it submits and clears it when the turn settles, so no extra state is
	// needed to tell a quiet moment from a busy one.
	const auto turn_running = turn_started != std::chrono::steady_clock::time_point{ };

	coordinator.set_activity( activity_verb( coordinator, turn_running ) );
}

auto key_scroll_rows( const mcode::tui::tty_session& session_tty,
	const mcode::tui::key_event::kind type ) -> std::optional< int > {
	switch ( type ) {
		case mcode::tui::key_event::kind::page_up: return -page_rows( session_tty );
		case mcode::tui::key_event::kind::page_down: return page_rows( session_tty );
		case mcode::tui::key_event::kind::mouse_scroll_up: return -WHEEL_SCROLL_ROWS;
		case mcode::tui::key_event::kind::mouse_scroll_down: return WHEEL_SCROLL_ROWS;
		default: return std::nullopt;
	}
}

auto scroll_view( mcode::tui::render_coordinator& coordinator, std::mutex& render_gate,
	const int rows ) -> void {
	const auto held = std::lock_guard< std::mutex >{ render_gate };

	coordinator.scroll_by( rows );
}

auto return_to_live( mcode::tui::render_coordinator& coordinator,
	std::mutex& render_gate ) -> void {
	const auto held = std::lock_guard< std::mutex >{ render_gate };

	coordinator.scroll_to_bottom( );
}

auto view_scrolled( const mcode::tui::render_coordinator& coordinator,
	std::mutex& render_gate ) -> bool {
	const auto held = std::lock_guard< std::mutex >{ render_gate };

	return coordinator.scrolled( );
}

auto handle_turn_key( mcode::tui::render_coordinator& coordinator,
	std::atomic< bool >& interrupted, const mcode::tui::tty_session& session_tty,
	const mcode::tui::key_event& key ) -> bool {
	// Both keys stop the turn, and the flag is read by the loop at its next step
	// boundary. Esc was the only one handled, so Ctrl+C -- which the session's
	// own banner advertises -- fell through to the live-view path and did
	// nothing, and the turn ran to completion on the worker thread.
	if ( key.type == mcode::tui::key_event::kind::escape ||
		key.type == mcode::tui::key_event::kind::interrupt ) {
		interrupted.store( true );

		return true;
	}

	const auto rows = key_scroll_rows( session_tty, key.type );

	if ( rows.has_value( ) ) {
		coordinator.scroll_by( *rows );

		return true;
	}

	const auto was_scrolled = coordinator.scrolled( );

	// Any other key returns to the live view, so a turn that keeps
	// streaming never strands the user in history.
	coordinator.scroll_to_bottom( );

	return was_scrolled;
}

auto translate_key( const mcode::tui::key_event& key )
	-> std::optional< mcode::tui::input_editor::key_event > {
	auto forwarded = mcode::tui::input_editor::key_event{ };

	switch ( key.type ) {
		case mcode::tui::key_event::kind::character:
			forwarded.type = mcode::tui::input_editor::key::character;
			forwarded.text = key.text;

			break;
		case mcode::tui::key_event::kind::backspace:
			forwarded.type = mcode::tui::input_editor::key::backspace;

			break;
		case mcode::tui::key_event::kind::delete_key:
			forwarded.type = mcode::tui::input_editor::key::delete_key;

			break;
		case mcode::tui::key_event::kind::left:
			forwarded.type = mcode::tui::input_editor::key::left;

			break;
		case mcode::tui::key_event::kind::right:
			forwarded.type = mcode::tui::input_editor::key::right;

			break;
		case mcode::tui::key_event::kind::up:
			forwarded.type = mcode::tui::input_editor::key::up;

			break;
		case mcode::tui::key_event::kind::down:
			forwarded.type = mcode::tui::input_editor::key::down;

			break;
		case mcode::tui::key_event::kind::home:
			forwarded.type = mcode::tui::input_editor::key::home;

			break;
		case mcode::tui::key_event::kind::end:
			forwarded.type = mcode::tui::input_editor::key::end;

			break;
		default:
			return std::nullopt;
	}

	return forwarded;
}
