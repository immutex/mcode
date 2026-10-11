#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/cli/approval.hxx"
#include "mcode/cli/slash.hxx"
#include "mcode/cli/repl_events.hxx"
#include "mcode/cli/repl_plain.hxx"
#include "mcode/cli/repl_turn.hxx"
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
#include "mcode/tui/editor.hxx"
#include "mcode/tui/frame.hxx"
#include "mcode/tui/mention.hxx"
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

	// Short: typing must echo between keys, and the resize check rides the timeout.
	inline constexpr std::uint32_t KEY_WAIT_MS = 250;

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
		std::fprintf( stderr, "mcode: %s\n\n",
			mcode::cli::argument_error_text( arguments )->c_str( ) );
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	auto queue = mcode::tui::event_queue{ };
	auto coordinator = mcode::tui::render_coordinator{ };

	// Serializes every `render_coordinator` access: the pump repaints on the main
	// thread while the approval presenter draws on the worker. Never held recursively.
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

	// Esc or Ctrl+C: the delta subscriber drops the rest of the response, and the
	// loop stops at its step boundary.
	auto interrupted = std::atomic< bool >{ false };

	loop.set_cancel_source( &interrupted );

	auto subscriptions = subscribe_event_feed( loop.bus( ), queue, interrupted );

	auto last_code = mcode::cli::exit_code::success;
	auto turn_done = std::atomic< bool >{ false };
	// libc++ lacks `stop_token` at every macOS deployment target, so `std::jthread` is unusable.
	auto worker = std::thread{ };


	auto editor = mcode::tui::input_editor{ };

	// A root that will not open degrades to an empty picker, not a failed session.
	const auto mention_root = parsed->working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed->working_directory };
	auto mention_space = mcode::workspace::open( mention_root );
	auto files = mcode::tui::mention_index{ mention_space ? &*mention_space : nullptr };

	auto exiting = false;

	const auto& commands = mcode::cli::builtin_commands( );
	auto palette = mcode::tui::slash_palette{ };

	// Owned here, not by the controller: `list_sessions` returns a fresh vector, so
	// a pointer to a local in the opener would dangle by the time the picker is drawn.
	auto session_picker_rows = std::vector< mcode::tui::slash_command >{ };

	auto palette_controller = mcode::cli::palette_controller{ { &commands, &files,
		&editor.history( ), &session_picker_rows } };

	// Restored on Esc so Ctrl+R does not lose a half-typed prompt.
	auto history_draft = std::string{ };

	const auto sync_palette = [ & ]( ) {
		palette_controller.refresh( palette, editor.text( ) );

		const auto held = std::lock_guard< std::mutex >{ render_gate };

		coordinator.set_palette( palette );
	};

	// Set only while a turn runs, so a finished turn's time stays on screen.
	auto turn_started = std::chrono::steady_clock::time_point{ };
	auto last_turn_elapsed_ms = std::uint64_t{ 0 };

	const auto runtime = mcode::cli::detail::repl_runtime{ .tty = &*session_tty,
		.coordinator = &coordinator, .editor = &editor, .queue = &queue, .loop = &loop,
		.turn = &turn, .render_gate = &render_gate, .turn_done = &turn_done,
		.approval_active = &approval_active, .interrupted = &interrupted,
		.worker = &worker, .last_code = &last_code, .turn_started = &turn_started,
		.last_turn_elapsed_ms = &last_turn_elapsed_ms };

	{
		const auto held = std::lock_guard< std::mutex >{ render_gate };

		auto opening = mcode::tui::session_opening{ };
		opening.version = mcode::VERSION;
		opening.platform = mcode::PLATFORM;
		opening.compiler = mcode::COMPILER;
		opening.permissive = loop.approval_mode( ) == "never";

		session_tty->write( mcode::tui::emit_opening( coordinator, opening ) );
	}

	show_prompt( runtime );

	while ( true ) {
		auto submitted = std::optional< std::string >{ };

		while ( !submitted ) {
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

					show_prompt( runtime );
				}

				show_prompt( runtime );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::exit ) {
				exiting = true;

				break;
			}

			if ( view_scrolled( coordinator, render_gate ) ) {
				const auto rows = key_scroll_rows( *session_tty, key.type );

				if ( rows.has_value( ) ) {
					scroll_view( coordinator, render_gate, *rows );
					show_prompt( runtime );

					continue;
				}

				return_to_live( coordinator, render_gate );
				show_prompt( runtime );
			} else if ( const auto rows = key_scroll_rows( *session_tty, key.type );
				rows.has_value( ) ) {
				scroll_view( coordinator, render_gate, *rows );
				show_prompt( runtime );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::ctrl_r ) {
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

				show_prompt( runtime );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::interrupt ) {
				auto clear = mcode::tui::input_editor::key_event{ };
				clear.type = mcode::tui::input_editor::key::interrupt;
				std::ignore = editor.handle( clear );

				palette_controller.reset( );
				history_draft.clear( );
				sync_palette( );
				show_prompt( runtime );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::escape ) {
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

					show_prompt( runtime );

					continue;
				}

				auto abandon = mcode::tui::input_editor::key_event{ };
				abandon.type = mcode::tui::input_editor::key::escape;
				std::ignore = editor.handle( abandon );

				sync_palette( );
				show_prompt( runtime );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::enter ) {
				const auto* row = palette.highlighted( );

				if ( row != nullptr && !mcode::cli::submits( palette_controller.source( ) ) ) {
					editor.set_text( mcode::cli::inserted_line( palette_controller.source( ),
						editor.text( ), *row ) );
					palette_controller.reset( );
					history_draft.clear( );
					sync_palette( );
					show_prompt( runtime );

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

			if ( palette.open && !palette.empty( ) ) {
				if ( key.type == mcode::tui::key_event::kind::up ) {
					palette.selected = palette.selected == 0
						? palette.matches.size( ) - 1 : palette.selected - 1;
					sync_palette( );
					show_prompt( runtime );

					continue;
				}

				if ( key.type == mcode::tui::key_event::kind::down ) {
					palette.selected = ( palette.selected + 1 ) % palette.matches.size( );
					sync_palette( );
					show_prompt( runtime );

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
						show_prompt( runtime );

						continue;
					}
				}
			}

			// A paste is inserted, never submitted: its newlines are content.
			if ( key.type == mcode::tui::key_event::kind::paste ) {
				editor.insert_text( key.text );

				sync_palette( );
				show_prompt( runtime );

				continue;
			}

			const auto forwarded = translate_key( key );

			if ( !forwarded.has_value( ) ) {
				continue;
			}

			std::ignore = editor.handle( *forwarded );
			sync_palette( );
			show_prompt( runtime );
		}

		if ( !submitted || submitted->empty( ) ) {
			if ( !submitted ) {
				exiting = true;

				break;
			}

			continue;
		}

		// A palette submission bypassed the editor, so its buffer is cleared here.
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

		// The echo still shows what the user typed: a returned prompt is instructions,
		// not a transcript line.
		auto prompt = *submitted;

		if ( match.is_command ) {
			const auto result = mcode::cli::run_command( match, loop, commands,
				session_extensions( ), &mention_root );

			if ( result.should_exit ) {
				exiting = true;

				break;
			}

			if ( result.open_mention ) {
				editor.set_text( "@" );
				palette_controller.reset( );
				sync_palette( );
				show_prompt( runtime );

				continue;
			}

			if ( result.open_session_picker ) {
				auto sessions = workspace_sessions( );

				if ( !sessions ) {
					{
						const auto held = std::lock_guard< std::mutex >{ render_gate };

						coordinator.queue_text( sessions.error( ).msg, mcode::tui::token::warn );
					}

					show_prompt( runtime );

					continue;
				}

				session_picker_rows = mcode::cli::session_rows( *sessions );

				if ( session_picker_rows.empty( ) ) {
					// An empty picker would look like a broken list, not an empty workspace.
					{
						const auto held = std::lock_guard< std::mutex >{ render_gate };

						coordinator.queue_text( "no sessions recorded for this workspace yet",
							mcode::tui::token::warn );
					}

					show_prompt( runtime );

					continue;
				}

				editor.set_text( "/resume " );
				palette_controller.open_sessions( palette, editor.text( ) );

				// `sync_palette` before `show_prompt`: only it pushes the opened palette to
				// the coordinator, so `show_prompt` alone repaints the stale closed one.
				sync_palette( );
				show_prompt( runtime );

				continue;
			}

			// Only between turns: the log and history are not safe to move under a running one.
			if ( result.start_new_session || result.adopt_session_id.has_value( ) ) {
				auto swapped = result.start_new_session
					? start_new_session( loop )
					: adopt_session( loop, *result.adopt_session_id );
				{
					const auto held = std::lock_guard< std::mutex >{ render_gate };

					if ( !swapped ) {
						coordinator.queue_text( swapped.error( ).msg, mcode::tui::token::warn );
					} else {
						// The coordinator has no clear, so a separator marks the session boundary.
						coordinator.queue_text( *swapped, mcode::tui::token::accent );
					}
				}

				show_prompt( runtime );

				continue;
			}

			if ( !result.output.empty( ) ) {
				const auto held = std::lock_guard< std::mutex >{ render_gate };

				coordinator.queue_text( result.output );
			}

			if ( !result.submit_prompt ) {
				show_prompt( runtime );

				continue;
			}

			prompt = *result.submit_prompt;
		}

		run_one_turn( runtime, *submitted, prompt );
	}

	// Handed back before anything else writes, or the terminal keeps eating the wheel.
	session_tty->set_mouse_reporting( false );

	if ( exiting ) {
		// Cleared first: the destructor restores the console, and the shell's prompt
		// would land on it.
		const auto cleared = mcode::tui::ansi_emitter{ session_tty->caps( ) }
			.clear_region( coordinator.painted_height( ) );

		session_tty->write( cleared );
	}

	return mcode::cli::to_int( last_code );
}
