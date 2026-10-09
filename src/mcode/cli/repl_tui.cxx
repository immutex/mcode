#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/cli/approval.hxx"
#include "mcode/cli/slash.hxx"
#include "mcode/cli/repl_events.hxx"
#include "mcode/cli/repl_plain.hxx"
#include "mcode/cli/repl_view.hxx"
#include "mcode/cli/session.hxx"
#include "mcode/cli/repl.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/core/version.hxx"
#include "mcode/tui/opening.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/mcp/connect.hxx"
#include "mcode/model/capabilities.hxx"
#include "mcode/model/http_client.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/perm/approval_headless.hxx"
#include "mcode/perm/approval_terminal.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/perm/store.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/skills/session_context.hxx"
#include "mcode/support/config.hxx"
#include "mcode/support/time.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/tools/register.hxx"
#include "mcode/tui/approval_tui.hxx"
#include "mcode/tui/editor.hxx"
#include "mcode/tui/frame.hxx"
#include "mcode/tui/mention.hxx"
#include "mcode/tui/notify.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

	inline constexpr auto PUMP_TICK_MS = 50;

	// Short, not a long block: typing echoes between keys, and the resize
	// check rides the timeout.
	inline constexpr std::uint32_t KEY_WAIT_MS = 250;

	inline constexpr std::string_view INTERRUPTED_NOTICE = "interrupted";

}

auto run_repl( const std::vector< std::string >& arguments ) -> int {
	const auto* requested = std::getenv( "MCODE_TUI" );
	const auto plain_requested = requested != nullptr &&
		std::string_view{ requested } == "plain";

	if ( plain_requested || !mcode::platform::terminal_size( ).has_value( ) ) {
		return run_plain_repl( arguments );
	}

	auto session_tty = mcode::tui::tty_session::create( );

	if ( !session_tty ) {
		return run_plain_repl( arguments );
	}

	auto parsed = mcode::cli::parse_exec_options( arguments );

	if ( !parsed || !parsed->unknown_arguments.empty( ) ) {
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	auto queue = mcode::tui::event_queue{ };
	auto coordinator = mcode::tui::render_coordinator{ };

	// Serializes every `render_coordinator` access. The pump repaints on the
	// main thread while the approval presenter draws on the worker thread, and
	// both mutate the same live region; two interleaved flushes diffed against
	// a buffer that no longer modelled the screen. Never held recursively:
	// `refresh` and `repaint` below assume their caller holds it.
	auto render_gate = std::mutex{ };

	{
		const auto held = std::lock_guard< std::mutex >{ render_gate };

		coordinator.set_capabilities( session_tty->caps( ) );

		const auto measured = session_tty->size( );

		coordinator.resize( static_cast< std::size_t >( measured.second ),
			static_cast< std::size_t >( measured.first ) );
	}

	auto approval_active = std::atomic< bool >{ false };
	auto approval = mcode::cli::make_approval_source( *session_tty, approval_active,
		render_gate, coordinator );

	auto built = build_interactive_loop( *parsed, &approval );

	if ( !built ) {
		std::fprintf( stderr, "mcode: %s\n", built.error( ).msg.c_str( ) );

		return mcode::cli::to_int( mcode::cli::exit_code_for( built.error( ).code ) );
	}

	auto& loop = *built;
	auto turn = mcode::cli::session{ loop };

	// Set by Esc or Ctrl+C while a turn runs, and read on two sides: the delta
	// subscriber on the loop thread drops the rest of the response rather than
	// appending to the answer the user chose to keep, and the loop itself reads
	// it at its next step boundary and stops the turn.
	auto interrupted = std::atomic< bool >{ false };

	loop.set_cancel_source( &interrupted );

	auto subscriptions = subscribe_event_feed( loop.bus( ), queue, interrupted );

	auto last_code = mcode::cli::exit_code::success;
	auto turn_done = std::atomic< bool >{ false };
	// libc++ lacks `stop_token` at every macOS deployment target, so `std::jthread` is unusable.
	auto worker = std::thread{ };

	// Caller holds `render_gate`: the flush and the write that consumes its
	// bytes must not be split, or the two writers interleave on the console.
	auto repaint = [&coordinator, &session_tty]( ) {
		const auto bytes = coordinator.flush( );

		if ( !bytes.empty( ) ) {
			session_tty->write( session_tty->caps( ).synchronized_output
				? mcode::tui::ansi_emitter{ session_tty->caps( ) }.synchronized( bytes )
				: bytes );
		}
	};

	auto editor = mcode::tui::input_editor{ };

	// The picker walks the workspace once and reuses the list for every
	// keystroke. A root that will not open degrades to an empty picker rather
	// than failing the session.
	const auto mention_root = parsed->working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed->working_directory };
	auto mention_space = mcode::workspace::open( mention_root );
	auto files = mcode::tui::mention_index{ mention_space ? &*mention_space : nullptr };

	auto exiting = false;

	const auto& commands = mcode::cli::builtin_commands( );
	auto palette = mcode::tui::slash_palette{ };

	// The `/resume` picker's rows, rebuilt each time the picker opens. Owned here
	// rather than by the controller: `list_sessions` returns a fresh vector, so
	// the controller's pointer has to outlive the call, and a local inside the
	// opener would dangle before the picker is drawn.
	auto session_picker_rows = std::vector< mcode::tui::slash_command >{ };

	// One palette, four sources: slash commands, the file picker, the Ctrl+R
	// history search, and the session picker. The rows, the filter and the
	// renderer are shared.
	auto palette_controller = mcode::cli::palette_controller{ { &commands, &files,
		&editor.history( ), &session_picker_rows } };

	// The input as it was before Ctrl+R took over the prompt, so Esc restores
	// it rather than losing what the user had half-typed.
	auto history_draft = std::string{ };

	const auto sync_palette = [ & ]( ) {
		palette_controller.refresh( palette, editor.text( ) );

		const auto held = std::lock_guard< std::mutex >{ render_gate };

		coordinator.set_palette( palette );
	};

	// Set only while a turn runs, so a finished turn's time stays on screen.
	auto turn_started = std::chrono::steady_clock::time_point{ };
	auto last_turn_elapsed_ms = std::uint64_t{ 0 };

	const auto show_prompt = [ & ]( ) {
		const auto held = std::lock_guard< std::mutex >{ render_gate };

		refresh( coordinator, loop, turn_started, last_turn_elapsed_ms );
		coordinator.set_prompt( editor.text( ), editor.flattened_cursor( ) );
		repaint( );
	};

	auto pump_until_done = [&]() {
		auto was_running = false;
		auto was_asking = false;

		while ( !turn_done.load( ) ) {
			// The approval prompt reads the same console on the worker thread,
			// so the key read stands down while it owns the screen. The check
			// and the read are not atomic with respect to each other; the
			// window is microseconds wide and the prompt has not been shown to
			// the user yet, so no answer can be lost in it.
			auto pressed = mcode::tui::key_event{ };
			const auto asking = approval_active.load( );

			if ( !asking ) {
				pressed = session_tty->read_key( PUMP_TICK_MS );
			} else {
				std::this_thread::sleep_for( std::chrono::milliseconds( PUMP_TICK_MS ) );
			}

			// The prompt appeared: a window that is not in front gets told,
			// because the question blocks until it is answered.
			if ( asking && !was_asking ) {
				mcode::tui::notify_terminal( "mcode", "approval needed" );
			}

			was_asking = asking;

			const auto drained = queue.drain( );
			auto applied = false;
			auto running = false;
			auto keyed = false;

			{
				const auto held = std::lock_guard< std::mutex >{ render_gate };

				if ( pressed.type != mcode::tui::key_event::kind::timeout ) {
					keyed = handle_turn_key( coordinator, interrupted, *session_tty, pressed );
				}

				for ( const auto& item : drained ) {
					coordinator.apply( item );
					applied = true;
				}

				running = !coordinator.state( ).tools.empty( );

				if ( applied || running || was_running || keyed ) {
					// The approval prompt owns the console while it is up. The
					// flag is read under the same lock the presenter sets it
					// under, so a repaint cannot pass this check and then race
					// the presenter's draw.
					if ( !approval_active.load( ) ) {
						coordinator.advance_tools( mcode::tui::monotonic_ms( ) );
						refresh( coordinator, loop, turn_started, last_turn_elapsed_ms );
						repaint( );
					}
				}
			}

			was_running = running;
		}

		const auto held = std::lock_guard< std::mutex >{ render_gate };

		for ( const auto& item : queue.drain( ) ) {
			coordinator.apply( item );
		}

		approval_active.store( false );
		refresh( coordinator, loop, turn_started, last_turn_elapsed_ms );
		repaint( );
	};

	{
		const auto held = std::lock_guard< std::mutex >{ render_gate };

		auto opening = mcode::tui::session_opening{ };
		opening.version = mcode::VERSION;
		opening.platform = mcode::PLATFORM;
		opening.compiler = mcode::COMPILER;
		opening.permissive = loop.approval_mode( ) == "never";

		session_tty->write( mcode::tui::emit_opening( coordinator, opening ) );
	}

	show_prompt( );

	while ( true ) {
		auto submitted = std::optional< std::string >{ };

		while ( !submitted ) {
			// A short poll, not a long block, so typing echoes between keys.
			const auto key = session_tty->read_key( KEY_WAIT_MS );

			if ( key.type == mcode::tui::key_event::kind::timeout ) {
				if ( session_tty->poll_resize( 0 ) ) {
					const auto measured_now = session_tty->size( );

					{
						const auto held = std::lock_guard< std::mutex >{ render_gate };

						coordinator.resize( static_cast< std::size_t >( measured_now.second ),
							static_cast< std::size_t >( measured_now.first ) );
						coordinator.invalidate( );

						// The region moved with the screen, so the cursor is re-parked first.
						session_tty->write( coordinator.park( ) );
					}

					show_prompt( );
				}

				show_prompt( );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::exit ) {
				exiting = true;

				break;
			}

			// A scroll key moves the viewport; any other key first returns to
			// the live view, so the prompt is never unreachable.
			if ( view_scrolled( coordinator, render_gate ) ) {
				const auto rows = key_scroll_rows( *session_tty, key.type );

				if ( rows.has_value( ) ) {
					scroll_view( coordinator, render_gate, *rows );
					show_prompt( );

					continue;
				}

				return_to_live( coordinator, render_gate );
				show_prompt( );
			} else if ( const auto rows = key_scroll_rows( *session_tty, key.type );
				rows.has_value( ) ) {
				scroll_view( coordinator, render_gate, *rows );
				show_prompt( );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::ctrl_r ) {
				// The reverse search takes over the prompt: what is already
				// typed becomes its filter, and the pre-search input is kept
				// so Esc restores it.
				if ( palette_controller.source( ) != mcode::cli::palette_source::history ) {
					history_draft = editor.text( );
					palette_controller.open_history( palette, history_draft );
				} else {
					palette_controller.refresh( palette, editor.text( ) );
				}

				{
					const auto held = std::lock_guard< std::mutex >{ render_gate };

					coordinator.set_palette( palette );
				}

				show_prompt( );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::interrupt ) {
				auto clear = mcode::tui::input_editor::key_event{ };
				clear.type = mcode::tui::input_editor::key::interrupt;
				std::ignore = editor.handle( clear );

				palette_controller.reset( );
				history_draft.clear( );
				sync_palette( );
				show_prompt( );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::escape ) {
				// Esc dismisses the picker without submitting; the typed text
				// stays so the user can keep editing it. Leaving the history
				// search also restores what was typed before Ctrl+R.
				if ( palette.open ) {
					const auto was_history =
						palette_controller.source( ) == mcode::cli::palette_source::history;

					palette.open = false;
					palette.matches.clear( );
					palette.selected = 0;
					palette_controller.reset( );

					if ( was_history && !history_draft.empty( ) ) {
						editor.set_text( history_draft );
					}

					history_draft.clear( );

					{
						const auto held = std::lock_guard< std::mutex >{ render_gate };

						coordinator.set_palette( palette );
					}

					show_prompt( );

					continue;
				}

				auto abandon = mcode::tui::input_editor::key_event{ };
				abandon.type = mcode::tui::input_editor::key::escape;
				std::ignore = editor.handle( abandon );

				sync_palette( );
				show_prompt( );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::enter ) {
				const auto* row = palette.highlighted( );

				// Only a slash command runs. A mention and a history entry are
				// inserted into the prompt: the model reads the file itself,
				// and a recalled command is usually edited before it is sent.
				if ( row != nullptr && !mcode::cli::submits( palette_controller.source( ) ) ) {
					editor.set_text( mcode::cli::inserted_line( palette_controller.source( ),
						editor.text( ), *row ) );
					palette_controller.reset( );
					history_draft.clear( );
					sync_palette( );
					show_prompt( );

					continue;
				}

				if ( row != nullptr ) {
					submitted = mcode::tui::submitted_line( palette, editor.text( ) );
					editor.push_history( *submitted );

					continue;
				}

				auto enter = mcode::tui::input_editor::key_event{ };
				enter.type = mcode::tui::input_editor::key::enter;
				submitted = editor.handle( enter );

				continue;
			}

			// While up, the palette owns the arrow keys and Tab, not the caret.
			if ( palette.open && !palette.empty( ) ) {
				if ( key.type == mcode::tui::key_event::kind::up ) {
					palette.selected = palette.selected == 0
						? palette.matches.size( ) - 1 : palette.selected - 1;
					sync_palette( );
					show_prompt( );

					continue;
				}

				if ( key.type == mcode::tui::key_event::kind::down ) {
					palette.selected = ( palette.selected + 1 ) % palette.matches.size( );
					sync_palette( );
					show_prompt( );

					continue;
				}

				if ( key.type == mcode::tui::key_event::kind::tab ) {
					const auto* row = palette.highlighted( );

					if ( row != nullptr ) {
						editor.set_text( mcode::cli::inserted_line(
							palette_controller.source( ), editor.text( ), *row ) );
						palette_controller.reset( );
						history_draft.clear( );
						sync_palette( );
						show_prompt( );

						continue;
					}
				}
			}

			// A paste is inserted, never submitted: its newlines are content,
			// and a multi-line paste that submitted line by line was the bug.
			if ( key.type == mcode::tui::key_event::kind::paste ) {
				editor.insert_text( key.text );

				sync_palette( );
				show_prompt( );

				continue;
			}

			const auto forwarded = translate_key( key );

			if ( !forwarded.has_value( ) ) {
				continue;
			}

			std::ignore = editor.handle( *forwarded );
			sync_palette( );
			show_prompt( );
		}

		if ( !submitted || submitted->empty( ) ) {
			if ( !submitted ) {
				exiting = true;

				break;
			}

			continue;
		}

		// A palette submission never passed through the editor, so the buffer
		// is cleared here; the editor's own Enter already did it.
		editor.reset( );

		palette.open = false;
		palette.matches.clear( );
		palette_controller.reset( );
		history_draft.clear( );

		{
			const auto held = std::lock_guard< std::mutex >{ render_gate };

			coordinator.set_palette( palette );
		}

		const auto match = mcode::cli::match_command( *submitted, commands );

		// What the turn runs. A plain submission runs what was typed; a command
		// that returned a prompt runs that instead. The echo below still prints
		// what the user typed, because a generated prompt is instructions rather
		// than a transcript line.
		auto prompt = *submitted;

		if ( match.is_command ) {
			const auto result = mcode::cli::run_command( match, loop, commands,
				session_extensions( ) );

			if ( result.should_exit ) {
				exiting = true;

				break;
			}

			if ( result.open_mention ) {
				editor.set_text( "@" );
				palette_controller.reset( );
				sync_palette( );
				show_prompt( );

				continue;
			}

			if ( result.open_session_picker ) {
				auto sessions = workspace_sessions( );

				if ( !sessions ) {
					{
						const auto held = std::lock_guard< std::mutex >{ render_gate };

						coordinator.queue_text( sessions.error( ).msg, mcode::tui::token::warn );
					}

					show_prompt( );

					continue;
				}

				session_picker_rows = mcode::cli::session_rows( *sessions );

				if ( session_picker_rows.empty( ) ) {
					// Opening an empty list would look like a picker with no rows
					// rather than a workspace with no sessions.
					{
						const auto held = std::lock_guard< std::mutex >{ render_gate };

						coordinator.queue_text( "no sessions recorded for this workspace yet",
							mcode::tui::token::warn );
					}

					show_prompt( );

					continue;
				}

				editor.set_text( "/resume " );
				palette_controller.open_sessions( palette, editor.text( ) );

				// `sync_palette`, not `show_prompt`: opening the picker changes the
				// palette's contents, and only `sync_palette` pushes them to the
				// coordinator. `show_prompt` alone repaints the stale closed palette
				// it already had, which is what made `/resume` look like it did
				// nothing.
				sync_palette( );
				show_prompt( );

				continue;
			}

			// The session swap. Only between turns -- the log, the budget and the
			// history are not safe to move under a running one, and this branch is
			// reached before the worker starts.
			if ( result.start_new_session || result.adopt_session_id.has_value( ) ) {
				auto swapped = result.start_new_session
					? start_new_session( loop )
					: adopt_session( loop, *result.adopt_session_id );
				{
					const auto held = std::lock_guard< std::mutex >{ render_gate };

					if ( !swapped ) {
						coordinator.queue_text( swapped.error( ).msg, mcode::tui::token::warn );
					} else {
						// A boundary, so the transcript above is not read as this
						// session's. The coordinator keeps the previous session's
						// rows and has no clear, so a separator is what tells them
						// apart.
						coordinator.queue_text( *swapped, mcode::tui::token::accent );
					}
				}

				show_prompt( );

				continue;
			}

			if ( !result.output.empty( ) ) {
				const auto held = std::lock_guard< std::mutex >{ render_gate };

				coordinator.queue_text( result.output );
			}

			// A command that only printed is done; one that asked for a turn
			// falls through to the same path a typed prompt takes.
			if ( !result.submit_prompt ) {
				show_prompt( );

				continue;
			}

			prompt = *result.submit_prompt;
		}

		{
			const auto held = std::lock_guard< std::mutex >{ render_gate };

			coordinator.queue_text(
				std::string{ mcode::tui::USER_GUTTER } + " " + *submitted,
				mcode::tui::token::accent );
		}

		show_prompt( );

		turn_done.store( false );
		interrupted.store( false );
		turn_started = std::chrono::steady_clock::now( );

		// The wheel is reported only while a turn runs: at the prompt the
		// terminal's own scrollback is the better target, and taking the wheel
		// there would freeze it.
		session_tty->set_mouse_reporting( true );

		worker = std::thread{ [ & ]( ) {
			last_code = turn.run_turn( prompt );
			turn_done.store( true );
		} };

		pump_until_done( );
		worker.join( );

		session_tty->set_mouse_reporting( false );

		// The turn is over: a window that is not in front gets told, and one
		// that is gets an invisible notification the terminal discards.
		mcode::tui::notify_terminal( "mcode",
			last_code == mcode::cli::exit_code::success ? "turn complete" : "turn failed" );

		last_turn_elapsed_ms = static_cast< std::uint64_t >( std::chrono::duration_cast<
			std::chrono::milliseconds >( std::chrono::steady_clock::now( )
				- turn_started ).count( ) );
		turn_started = std::chrono::steady_clock::time_point{ };

		if ( interrupted.exchange( false ) ) {
			last_code = mcode::cli::exit_code::interrupted;

			const auto held = std::lock_guard< std::mutex >{ render_gate };

			coordinator.queue_text( std::string{ INTERRUPTED_NOTICE },
				mcode::tui::token::warn );
		}

		// Draining during the turn would race the approval prompt reading the same console.
		while ( true ) {
			const auto pressed = session_tty->read_key( 0 );

			if ( pressed.type == mcode::tui::key_event::kind::timeout ) {
				break;
			}
		}

		show_prompt( );
	}

	// The destructor restores the console, but the wheel must be handed back
	// before anything else writes, or the terminal keeps eating it.
	session_tty->set_mouse_reporting( false );

	if ( exiting ) {
		// Clear before the destructor restores the console, or the shell's prompt lands on it.
		const auto cleared = mcode::tui::ansi_emitter{ session_tty->caps( ) }
			.clear_region( coordinator.painted_height( ) );

		session_tty->write( cleared );
	}

	return mcode::cli::to_int( last_code );
}
