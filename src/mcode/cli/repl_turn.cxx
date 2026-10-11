#include "repl_turn.hxx"

#include <chrono>
#include <mutex>
#include <string>

#include "mcode/cli/repl_view.hxx"
#include "mcode/tui/frame.hxx"
#include "mcode/tui/notify.hxx"

namespace mcode::cli::detail {

	namespace {

		// Short: typing must echo between keys, and the resize check rides the timeout.
		inline constexpr std::uint32_t PUMP_TICK_MS = 50;

		inline constexpr std::string_view INTERRUPTED_NOTICE = "interrupted";

	}

	auto repaint( const repl_runtime& runtime ) -> void {
		const auto bytes = runtime.coordinator->flush( );

		if ( bytes.empty( ) ) {
			return;
		}

		const auto caps = runtime.tty->caps( );

		runtime.tty->write( caps.synchronized_output
			? mcode::tui::ansi_emitter{ caps }.synchronized( bytes )
			: bytes );
	}

	auto show_prompt( const repl_runtime& runtime ) -> void {
		const auto held = std::lock_guard< std::mutex >{ *runtime.render_gate };

		refresh( *runtime.coordinator, *runtime.loop, *runtime.turn_started,
			*runtime.last_turn_elapsed_ms );
		runtime.coordinator->set_prompt( runtime.editor->text( ),
			runtime.editor->flattened_cursor( ) );
		repaint( runtime );
	}

	auto run_one_turn( const repl_runtime& runtime, const std::string_view submitted_text,
		const std::string_view prompt_text ) -> void {
		{
			const auto held = std::lock_guard< std::mutex >{ *runtime.render_gate };

			runtime.coordinator->queue_text(
				std::string{ mcode::tui::USER_GUTTER } + " " + std::string{ submitted_text },
				mcode::tui::token::accent );
		}

		show_prompt( runtime );

		runtime.turn_done->store( false );
		runtime.interrupted->store( false );
		*runtime.turn_started = std::chrono::steady_clock::now( );

		// Only while a turn runs: taking the wheel at the prompt would freeze the scrollback.
		runtime.tty->set_mouse_reporting( true );

		*runtime.worker = std::thread{ [ &runtime, prompt_text ]( ) {
			*runtime.last_code = runtime.turn->run_turn( std::string{ prompt_text } );
			runtime.turn_done->store( true );
		} };

		auto was_running = false;
		auto was_asking = false;

		while ( !runtime.turn_done->load( ) ) {
			// The approval prompt reads the same console on the worker thread, so the key
			// read stands down while it owns the screen; the tiny race loses no answer.
			auto pressed = mcode::tui::key_event{ };
			const auto asking = runtime.approval_active->load( );

			if ( asking ) {
				std::this_thread::sleep_for( std::chrono::milliseconds( PUMP_TICK_MS ) );
			} else {
				pressed = runtime.tty->read_key( PUMP_TICK_MS );
			}

			// The question blocks until answered, so a window that is not in front gets told.
			if ( asking && !was_asking ) {
				mcode::tui::notify_terminal( "mcode", "approval needed" );
			}

			was_asking = asking;

			const auto drained = runtime.queue->drain( );
			auto applied = false;
			auto running = false;
			auto keyed = false;

			{
				const auto held = std::lock_guard< std::mutex >{ *runtime.render_gate };

				if ( pressed.type != mcode::tui::key_event::kind::timeout ) {
					keyed = handle_turn_key( *runtime.coordinator, *runtime.interrupted,
						*runtime.tty, pressed );
				}

				for ( const auto& item : drained ) {
					runtime.coordinator->apply( item );
					applied = true;
				}

				running = !runtime.coordinator->state( ).tools.empty( );

				if ( applied || running || was_running || keyed ) {
					// Read under the same lock the presenter sets it under, so a repaint
					// cannot race its draw.
					if ( !runtime.approval_active->load( ) ) {
						runtime.coordinator->advance_tools( mcode::tui::monotonic_ms( ) );
						refresh( *runtime.coordinator, *runtime.loop, *runtime.turn_started,
							*runtime.last_turn_elapsed_ms );
						repaint( runtime );
					}
				}
			}

			was_running = running;
		}

		{
			const auto held = std::lock_guard< std::mutex >{ *runtime.render_gate };

			for ( const auto& item : runtime.queue->drain( ) ) {
				runtime.coordinator->apply( item );
			}

			runtime.approval_active->store( false );
			refresh( *runtime.coordinator, *runtime.loop, *runtime.turn_started,
				*runtime.last_turn_elapsed_ms );
			repaint( runtime );
		}

		runtime.worker->join( );

		runtime.tty->set_mouse_reporting( false );

		// Tells a window that is not in front; a focused one discards it.
		mcode::tui::notify_terminal( "mcode",
			*runtime.last_code == mcode::cli::exit_code::success
				? "turn complete" : "turn failed" );

		*runtime.last_turn_elapsed_ms = static_cast< std::uint64_t >(
			std::chrono::duration_cast< std::chrono::milliseconds >(
				std::chrono::steady_clock::now( ) - *runtime.turn_started ).count( ) );
		*runtime.turn_started = std::chrono::steady_clock::time_point{ };

		if ( runtime.interrupted->exchange( false ) ) {
			*runtime.last_code = mcode::cli::exit_code::interrupted;

			const auto held = std::lock_guard< std::mutex >{ *runtime.render_gate };

			runtime.coordinator->queue_text( std::string{ INTERRUPTED_NOTICE },
				mcode::tui::token::warn );
		}

		// Draining during the turn would race the approval prompt reading the same console.
		while ( true ) {
			const auto pressed = runtime.tty->read_key( 0 );

			if ( pressed.type == mcode::tui::key_event::kind::timeout ) {
				break;
			}
		}

		show_prompt( runtime );
	}

}
