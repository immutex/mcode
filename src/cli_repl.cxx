#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "cli_approval.hxx"
#include "mcode/cli/slash.hxx"
#include "cli_repl_events.hxx"
#include "cli_repl_plain.hxx"
#include "cli_repl_view.hxx"
#include "cli_session.hxx"
#include "mcode/cli/repl.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/core/version.hxx"
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

	// Set by Esc while a turn runs and read by the delta subscriber on the
	// loop thread: after it is set, the rest of the response is dropped rather
	// than appended to the answer the user chose to keep.
	auto interrupted = std::atomic< bool >{ false };

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

	// One palette, three sources: slash commands, the file picker, and the
	// Ctrl+R history search. The rows, the filter and the renderer are shared.
	auto palette_controller = mcode::cli::palette_controller{ { &commands, &files,
		&editor.history( ) } };

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

		// The header is committed before the region is reserved, so it lands in
		// scrollback as the first thing in the session rather than scrolling away
		// with the first turn. It also fills the rows that `reserve` would
		// otherwise leave blank: pushing the region to the bottom with bare
		// newlines is what made the opening screen read as empty space.
		auto banner = std::vector< mcode::tui::styled_line >{ };

		const auto banner_line = [ & ]( const std::string_view text,
			const mcode::tui::token color ) {
			auto line = mcode::tui::styled_line{ };
			line.push_back( { std::string{ text }, color, mcode::tui::token::none,
				false, false, false } );
			banner.push_back( std::move( line ) );
		};

		banner_line( "", mcode::tui::token::none );
		banner_line( "   mcode " + std::string{ mcode::VERSION },
			mcode::tui::token::accent );
		banner_line( "   " + std::string{ mcode::PLATFORM } + ", "
			+ std::string{ mcode::COMPILER }, mcode::tui::token::muted );
		banner_line( "", mcode::tui::token::none );

		// The approval boundary is stated here rather than on stderr before the
		// session starts, where it scrolled out of sight. A permissive default is
		// only defensible if what still holds is visible while it holds.
		if ( loop.approval_mode( ) == "never" ) {
			banner_line( "   edits and commands run without prompting", mcode::tui::token::warn );
			banner_line( "   the hard-deny floor and permissions.deny still apply",
				mcode::tui::token::muted );
		}

		banner_line( "", mcode::tui::token::none );
		banner_line( "   /help for commands, @ to mention a file, Ctrl+C to interrupt",
			mcode::tui::token::muted );
		banner_line( "", mcode::tui::token::none );

		coordinator.queue_block( std::move( banner ) );

		session_tty->write( coordinator.flush( ) );
		session_tty->write( coordinator.reserve( ) );
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

		if ( match.is_command ) {
			const auto result = mcode::cli::run_command( match, loop, commands );

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

			{
				const auto held = std::lock_guard< std::mutex >{ render_gate };

				coordinator.queue_text( result.output );
			}

			show_prompt( );

			continue;
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
			last_code = turn.run_turn( *submitted );
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

		if ( last_code == mcode::cli::exit_code::interrupted ) {
			continue;
		}
	}

	// The destructor restores the console, but the wheel must be handed back
	// before anything else writes, or the terminal keeps eating it.
	session_tty->set_mouse_reporting( false );

	if ( exiting ) {
		// Clear before the destructor restores the console, or the shell's prompt lands on it.
		const auto cleared = mcode::tui::ansi_emitter{ session_tty->caps( ) }
			.clear_region( mcode::tui::LIVE_REGION_ROWS );

		session_tty->write( cleared );
	}

	return mcode::cli::to_int( last_code );
}
