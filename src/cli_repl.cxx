// The interactive session: the no-subcommand surface. Split from
// cli_commands.cxx, which owns the headless `exec` handler; the two share
// no state, only the platform name.
#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "cli_session.hxx"
#include "mcode/cli/repl.hxx"
#include "mcode/core/registry.hxx"
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
#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/tools/register.hxx"
#include "mcode/tui/approval_tui.hxx"
#include "mcode/tui/editor.hxx"
#include "mcode/tui/frame.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>



// The interactive session. The construction sequence is run_exec's, minus the
// single-run JSON stream and the single run( ) call: the loop stays alive so
// consecutive turns share history, the remember store and the session rules.
// Returns the process exit code.
// The interactive surface. With a terminal: a tty_session in raw mode, the
// multi-line editor producing input lines, the render coordinator consuming
// the event queue the loop's bus handlers feed, and the turn running on a
// worker thread so a slow tool call cannot freeze the repaint. The loop
// thread stays the only bus publisher -- its handlers push copies into the
// queue, and only the render side drains it.
//
// Without a terminal, or under MCODE_TUI=plain, the session falls back to
// the plain line reader: an explicit branch, not a silent degradation.
auto run_repl( const std::vector< std::string >& arguments ) -> int {
	const auto* requested = std::getenv( "MCODE_TUI" );
	const auto plain_requested = requested != nullptr &&
		std::string_view{ requested } == "plain";

	const auto plain_reader = []( ) -> std::optional< std::string > {
		auto line = std::string{ };

		if ( !std::getline( std::cin, line ) ) {
			return std::nullopt;
		}

		while ( !line.empty( ) && line.back( ) == '\r' ) {
			line.pop_back( );
		}

		return line;
	};

	if ( plain_requested || !mcode::platform::terminal_size( ).has_value( ) ) {
		return mcode::cli::run_session( arguments, plain_reader, build_interactive_loop );
	}

	auto session_tty = mcode::tui::tty_session::create( );

	if ( !session_tty ) {
		// Raw mode unavailable: the plain reader still gives a working
		// session rather than a broken one.
		return mcode::cli::run_session( arguments, plain_reader, build_interactive_loop );
	}

	auto parsed = mcode::cli::parse_exec_options( arguments );

	if ( !parsed || !parsed->unknown_arguments.empty( ) ) {
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	// The approval prompt reads through the same raw-mode line reader the
	// editor uses, so it works under raw mode where std::getline would
	// block forever on \r.
	//
	// `approval_active` stops the pump repainting while the prompt is on
	// screen. Both threads would otherwise write to the console, and the
	// 80 ms repaint would paint the live region over the question the user is
	// being asked to answer.
	auto queue = mcode::tui::event_queue{ };
	auto coordinator = mcode::tui::render_coordinator{ };
	coordinator.set_capabilities( session_tty->caps( ) );

	const auto measured = session_tty->size( );
	coordinator.resize( static_cast< std::size_t >( measured.second ),
		static_cast< std::size_t >( measured.first ) );

	auto approval_active = std::atomic< bool >{ false };

	// The prompt block's height, shared with the answer reader. The question is
	// drawn from the parked row upwards, so it is the rows plus the row the
	// answer is typed on.
	auto approval_rows = std::size_t{ 0 };

	auto approval = mcode::tui::ui_approval_source{
		// The answer is read, and then the question is cleared. Both halves sit
		// here rather than in the presenter because the presenter runs BEFORE
		// the read: drawing and clearing it there erased the question before
		// the user could see what they were being asked to approve.
		//
		// Clearing is what stops the next commit from scrolling the block above
		// the live region, where the region's fixed-height clear cannot reach
		// it and the previous question stays on screen above the new one.
		//
		// A re-prompting answer ("?" or anything unrecognised) leaves the block
		// up, because `ask` loops and reads again without re-presenting.
		[ &session_tty, &approval_active, &coordinator, &approval_rows ]( ) {
			auto answer = session_tty->read_line( 600'000 );

			const auto settled = answer && answer->size( ) == 1 &&
				( *answer == "y" || *answer == "a" || *answer == "n" || *answer == "d" );

			// An unrecognised answer and "?" both re-prompt inside `ask`, which
			// reads again without re-presenting -- so the block stays up for
			// those. Everything else ends the ask, including closed input,
			// which resolves to refused; leaving the block up there would
			// freeze the pump for the rest of the turn.
			if ( answer && !settled ) {
				return answer;
			}

			session_tty->write(
				mcode::tui::ansi_emitter{ session_tty->caps( ) }
					.clear_region( approval_rows ) +
				coordinator.park( ) );

			// The pump repaints again from here. Holding this until the turn
			// ended left the answer invisible for the rest of the turn, because
			// the pump is what draws the streaming text.
			approval_active.store( false );

			return answer;
		},
		[ &session_tty, &approval_active, &coordinator, &approval_rows ](
			const std::vector< std::string >& rows ) {
			approval_active.store( true );
			approval_rows = rows.size( ) + 1;

			// The live region is cleared so the question has the screen to
			// itself; the pump restores it once the answer arrives. The
			// coordinator is told, because it wrote these bytes outside
			// `flush` and its tracked frame no longer matches the screen.
			coordinator.invalidate( );

			auto out = coordinator.park( );
			out += mcode::tui::ansi_emitter{ session_tty->caps( ) }
				.clear_region( mcode::tui::LIVE_REGION_ROWS );
			out += mcode::tui::ansi_emitter{ session_tty->caps( ) }
				.region_top( mcode::tui::LIVE_REGION_ROWS );

			for ( const auto& row : rows ) {
				out += row;
				out += "\r\n";
			}

			out += "> ";
			session_tty->write( out );
		} };

	auto built = build_interactive_loop( *parsed, &approval );

	if ( !built ) {
		std::fprintf( stderr, "mcode: %s\n", built.error( ).msg.c_str( ) );

		return mcode::cli::to_int( mcode::cli::exit_code_for( built.error( ).code ) );
	}

	auto& loop = *built;
	auto turn = mcode::cli::session{ loop };

	// The bus handlers run on the loop thread (the worker below) and push
	// copies into the queue; the render side is the only reader.
	// The payload is a JSON object and the renderer wants the one string a
	// human reads. Forwarding the raw payload put `{"text":"I"}` on screen
	// instead of the text: every event type has its own field, and none was
	// extracted.
	const auto display_text = []( const mcode::events::kind type,
		const std::string& payload_json ) -> std::string {
		auto payload = mcode::json::document::parse( payload_json );

		if ( !payload ) {
			return { };
		}

		const auto field = [ & ]( const std::string_view pointer ) -> std::string {
			const auto found = payload->pointer_string( pointer );

			return found ? *found : std::string{ };
		};

		switch ( type ) {
			case mcode::events::kind::assistant_delta:
				return field( "/text" );
			case mcode::events::kind::tool_call:
			case mcode::events::kind::tool_result:
				return field( "/tool" );
			default:
				return { };
		}
	};

	const auto feed = [ &queue, &display_text ]( mcode::tui::event_queue::kind target,
		const mcode::events::kind source ) {
		return [ &queue, &display_text, target, source ](
			const mcode::events::event& value ) {
			auto item = mcode::tui::event_queue::item{ };
			item.type = target;
			item.text = display_text( source, value.payload_json );

			queue.push( std::move( item ) );
		};
	};

	auto subscriptions = std::vector< mcode::events::bus::subscription_id >{ };
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::assistant_delta, feed( mcode::tui::event_queue::kind::assistant_delta,
			mcode::events::kind::assistant_delta ) ) );
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::tool_call, feed( mcode::tui::event_queue::kind::tool_start,
			mcode::events::kind::tool_call ) ) );
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::tool_result, feed( mcode::tui::event_queue::kind::tool_end,
			mcode::events::kind::tool_result ) ) );
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::turn_start, feed( mcode::tui::event_queue::kind::turn_start,
			mcode::events::kind::turn_start ) ) );
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::turn_end, feed( mcode::tui::event_queue::kind::turn_end,
			mcode::events::kind::turn_end ) ) );

	auto last_code = mcode::cli::exit_code::success;
	auto turn_done = std::atomic< bool >{ false };
	// `std::thread`, not `std::jthread`: jthread needs `stop_token`, which
	// libc++ does not provide at every macOS deployment target, and this code
	// joins explicitly anyway -- so the auto-join was never the reason.
	auto worker = std::thread{ };

	auto repaint = [&coordinator, &session_tty]( ) {
		const auto bytes = coordinator.flush( );

		if ( !bytes.empty( ) ) {
			session_tty->write( session_tty->caps( ).synchronized_output
				? mcode::tui::ansi_emitter{ session_tty->caps( ) }.synchronized( bytes )
				: bytes );
		}
	};

	auto editor = mcode::tui::input_editor{ };
	auto exiting = false;

	// The status line's clock. `turn_started` is set only while a turn runs, so
	// a finished turn's time stays on screen until the next one begins rather
	// than snapping to 0.0s the moment the answer arrives.
	auto turn_started = std::chrono::steady_clock::time_point{ };
	auto last_turn_elapsed_ms = std::uint64_t{ 0 };

	// The meter, read from the loop's budget. The pump calls this as well as
	// `show_prompt`, so tokens, cost and the clock move while a turn runs
	// rather than freezing at whatever the previous turn left behind.
	const auto refresh = [ & ]( ) {
		const auto& budget = loop.budget( );
		const auto elapsed = turn_started == std::chrono::steady_clock::time_point{ }
			? last_turn_elapsed_ms
			: static_cast< std::uint64_t >( std::chrono::duration_cast<
				std::chrono::milliseconds >( std::chrono::steady_clock::now( )
					- turn_started ).count( ) );

		coordinator.set_meter( std::string{ loop.model_name( ) }, budget.tokens_used,
			budget.usd_used, elapsed );
	};

	// Draws the prompt row and repaints. Called before the first key so the
	// terminal is not blank, and after every edit so typing echoes.
	const auto show_prompt = [ & ]( ) {
		refresh( );
		coordinator.set_prompt( editor.text( ), editor.flattened_cursor( ) );
		repaint( );
	};

	auto pump_until_done = [&]() {
		while ( !turn_done.load( ) ) {
			for ( const auto& item : queue.drain( ) ) {
				coordinator.apply( item );
			}

			// The approval prompt owns the console while it is up.
			if ( !approval_active.load( ) ) {
				refresh( );
				repaint( );
			}

			std::this_thread::sleep_for( std::chrono::milliseconds( 80 ) );
		}

		for ( const auto& item : queue.drain( ) ) {
			coordinator.apply( item );
		}

		approval_active.store( false );
		refresh( );
		repaint( );
	};

	// Scroll the region into existence so the parked cursor is on its last
	// row, which is what the relative addressing assumes.
	session_tty->write( coordinator.reserve( ) );

	show_prompt( );

	while ( true ) {
		// The editor owns the prompt: keys go through it, so multi-line
		// editing, the history ring and the ghost-text suggestion are live.
		// The prompt row renders the pending input between keys.
		auto submitted = std::optional< std::string >{ };

		while ( !submitted ) {
			// A short poll, not a long block: the prompt repaints between
			// keys, so typing echoes and an idle session stays responsive.
			const auto key = session_tty->read_key( 250 );

			if ( key.type == mcode::tui::key_event::kind::timeout ) {
				// A resize invalidates every row the diff addresses, so the
				// region is re-measured and repainted from scratch. Without
				// this the prompt drew at the old coordinates after any
				// terminal resize and the region stayed corrupt.
				if ( session_tty->resized( ) ) {
					const auto measured_now = session_tty->size( );

					coordinator.resize( static_cast< std::size_t >( measured_now.second ),
						static_cast< std::size_t >( measured_now.first ) );
					show_prompt( );
				}

				// The wait elapsed with no key. Idle is not exit: returning
				// here ended the session whenever the user paused.
				show_prompt( );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::exit ) {
				// Ctrl+D ends the session; the editor's exit flag tracks it.
				exiting = true;

				break;
			}

			if ( key.type == mcode::tui::key_event::kind::interrupt ) {
				// Ctrl+C clears the pending input and returns to the prompt;
				// the session continues.
				auto clear = mcode::tui::input_editor::key_event{ };
				clear.type = mcode::tui::input_editor::key::interrupt;
				std::ignore = editor.handle( clear );

				show_prompt( );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::escape ) {
				// Escape abandons the pending input. It used to end the whole
				// session, which lost the conversation to one stray keypress.
				auto abandon = mcode::tui::input_editor::key_event{ };
				abandon.type = mcode::tui::input_editor::key::escape;
				std::ignore = editor.handle( abandon );

				show_prompt( );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::enter ) {
				auto enter = mcode::tui::input_editor::key_event{ };
				enter.type = mcode::tui::input_editor::key::enter;
				submitted = editor.handle( enter );

				continue;
			}

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
					continue;
			}

			std::ignore = editor.handle( forwarded );
			show_prompt( );
		}

		if ( !submitted || submitted->empty( ) ) {
			if ( !submitted ) {
				exiting = true;

				break;
			}

			continue;
		}

		// The editor cleared itself on submit, so the prompt row is repainted
		// before the turn starts -- otherwise the submitted text stayed on
		// screen for the whole turn as if it were still being edited.
		show_prompt( );

		turn_done.store( false );
		turn_started = std::chrono::steady_clock::now( );

		worker = std::thread{ [ & ]( ) {
			last_code = turn.run_turn( *submitted );
			turn_done.store( true );
		} };

		pump_until_done( );
		worker.join( );

		last_turn_elapsed_ms = static_cast< std::uint64_t >( std::chrono::duration_cast<
			std::chrono::milliseconds >( std::chrono::steady_clock::now( )
				- turn_started ).count( ) );
		turn_started = std::chrono::steady_clock::time_point{ };

		// Type-ahead is discarded, once, here -- not while the turn runs.
		//
		// The console buffers keystrokes, so everything typed during the turn
		// arrived in one burst when it ended: the editor took the characters
		// and any Enter among them submitted whatever had accumulated. Draining
		// on the main thread DURING the turn would race the approval prompt,
		// which reads the same console from the worker thread, so the flush
		// happens only after the worker has stopped.
		while ( true ) {
			const auto pressed = session_tty->read_key( 0 );

			if ( pressed.type == mcode::tui::key_event::kind::timeout ) {
				break;
			}
		}

		if ( last_code == mcode::cli::exit_code::interrupted ) {
			continue;
		}
	}

	if ( exiting ) {
		// Clear the live region before the destructor restores the console
		// mode. Leaving it painted made the shell's own prompt appear on top
		// of the status line and the stale input row.
		const auto cleared = mcode::tui::ansi_emitter{ session_tty->caps( ) }
			.clear_region( mcode::tui::LIVE_REGION_ROWS );

		session_tty->write( cleared );
	}

	return mcode::cli::to_int( last_code );
}
