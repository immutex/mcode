#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "cli_approval.hxx"
#include "mcode/cli/slash.hxx"
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
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>



namespace {

	inline constexpr auto PUMP_TICK_MS = 50;

}

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
		return mcode::cli::run_session( arguments, plain_reader, build_interactive_loop );
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
			case mcode::events::kind::assistant_thinking:
				return field( "/text" );
			case mcode::events::kind::tool_call:
			case mcode::events::kind::tool_result:
				return field( "/tool" );
			default:
				return { };
		}
	};

	const auto tool_target = []( const std::string& payload_json ) -> std::string {
		auto payload = mcode::json::document::parse( payload_json );

		if ( !payload ) {
			return { };
		}

		for ( const auto* pointer : { "/args/path", "/args/command" } ) {
			if ( const auto found = payload->pointer_string( pointer ) ) {
				return *found;
			}
		}

		return { };
	};

	const auto feed = [ &queue, &display_text, &tool_target ](
		mcode::tui::event_queue::kind target, const mcode::events::kind source ) {
		return [ &queue, &display_text, &tool_target, target, source ](
			const mcode::events::event& value ) {
			auto item = mcode::tui::event_queue::item{ };
			item.type = target;
			item.text = display_text( source, value.payload_json );
			item.stamp_ms = mcode::tui::monotonic_ms( );

			if ( source == mcode::events::kind::tool_call ) {
				item.target = tool_target( value.payload_json );
			}

			queue.push( std::move( item ) );
		};
	};

	auto subscriptions = std::vector< mcode::events::bus::subscription_id >{ };
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::assistant_delta, feed( mcode::tui::event_queue::kind::assistant_delta,
			mcode::events::kind::assistant_delta ) ) );
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::assistant_thinking,
		feed( mcode::tui::event_queue::kind::thinking_delta,
			mcode::events::kind::assistant_thinking ) ) );
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
	auto exiting = false;

	const auto& commands = mcode::cli::builtin_commands( );
	auto palette = mcode::tui::slash_palette{ };

	const auto refresh_palette = [ & ]( ) {
		mcode::cli::refresh_palette( palette, commands, editor.text( ) );

		const auto held = std::lock_guard< std::mutex >{ render_gate };

		coordinator.set_palette( palette );
	};

	// Set only while a turn runs, so a finished turn's time stays on screen.
	auto turn_started = std::chrono::steady_clock::time_point{ };
	auto last_turn_elapsed_ms = std::uint64_t{ 0 };

	// Caller holds `render_gate`.
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

	const auto show_prompt = [ & ]( ) {
		const auto held = std::lock_guard< std::mutex >{ render_gate };

		refresh( );
		coordinator.set_prompt( editor.text( ), editor.flattened_cursor( ) );
		repaint( );
	};

	auto pump_until_done = [&]() {
		auto was_running = false;

		while ( !turn_done.load( ) ) {
			const auto drained = queue.drain( );
			auto applied = false;
			auto running = false;

			{
				const auto held = std::lock_guard< std::mutex >{ render_gate };

				for ( const auto& item : drained ) {
					coordinator.apply( item );
					applied = true;
				}

				running = !coordinator.state( ).tools.empty( );

				if ( applied || running || was_running ) {
					// The approval prompt owns the console while it is up. The
					// flag is read under the same lock the presenter sets it
					// under, so a repaint cannot pass this check and then race
					// the presenter's draw.
					if ( !approval_active.load( ) ) {
						coordinator.advance_tools( mcode::tui::monotonic_ms( ) );
						refresh( );
						repaint( );
					}
				}
			}

			was_running = running;

			std::this_thread::sleep_for( std::chrono::milliseconds( PUMP_TICK_MS ) );
		}

		const auto held = std::lock_guard< std::mutex >{ render_gate };

		for ( const auto& item : queue.drain( ) ) {
			coordinator.apply( item );
		}

		approval_active.store( false );
		refresh( );
		repaint( );
	};

	{
		const auto held = std::lock_guard< std::mutex >{ render_gate };

		session_tty->write( coordinator.reserve( ) );
	}

	show_prompt( );

	while ( true ) {
		auto submitted = std::optional< std::string >{ };

		while ( !submitted ) {
			// A short poll, not a long block, so typing echoes between keys.
			const auto key = session_tty->read_key( 250 );

			if ( key.type == mcode::tui::key_event::kind::timeout ) {
				if ( session_tty->resized( ) ) {
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

			if ( key.type == mcode::tui::key_event::kind::interrupt ) {
				auto clear = mcode::tui::input_editor::key_event{ };
				clear.type = mcode::tui::input_editor::key::interrupt;
				std::ignore = editor.handle( clear );

				show_prompt( );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::escape ) {
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

			// While up, the palette owns the arrow keys and Tab, not the caret.
			if ( palette.open && !palette.empty( ) ) {
				if ( key.type == mcode::tui::key_event::kind::up ) {
					palette.selected = palette.selected == 0
						? palette.matches.size( ) - 1 : palette.selected - 1;
					refresh_palette( );
					show_prompt( );

					continue;
				}

				if ( key.type == mcode::tui::key_event::kind::down ) {
					palette.selected = ( palette.selected + 1 ) % palette.matches.size( );
					refresh_palette( );
					show_prompt( );

					continue;
				}

				if ( key.type == mcode::tui::key_event::kind::tab ) {
					const auto& chosen = palette.matches[ palette.selected ];
					editor.set_text( mcode::tui::completed_command( chosen.name ) );
					refresh_palette( );
					show_prompt( );

					continue;
				}
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
			refresh_palette( );
			show_prompt( );
		}

		if ( !submitted || submitted->empty( ) ) {
			if ( !submitted ) {
				exiting = true;

				break;
			}

			continue;
		}

		palette.open = false;
		palette.matches.clear( );

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

		// Draining during the turn would race the approval prompt reading the same console.
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
		// Clear before the destructor restores the console, or the shell's prompt lands on it.
		const auto cleared = mcode::tui::ansi_emitter{ session_tty->caps( ) }
			.clear_region( mcode::tui::LIVE_REGION_ROWS );

		session_tty->write( cleared );
	}

	return mcode::cli::to_int( last_code );
}
