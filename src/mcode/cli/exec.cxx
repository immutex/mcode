#include "mcode/cli/exec.hxx"
#include "exec_internal.hxx"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "mcode/agent/loop.hxx"
#include "mcode/agent/message_json.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/proc/process.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/parse.hxx"
#include "mcode/support/time.hxx"

namespace mcode::cli {

	auto to_int( const exit_code code ) noexcept -> int {
		return static_cast< int >( code );
	}

	auto exit_code_for( const errc code ) -> exit_code {
		switch ( code ) {
			case errc::ok: return exit_code::success;
			case errc::cancelled: return exit_code::interrupted;
			case errc::budget_exhausted: return exit_code::budget_exhausted;
			case errc::config:
			case errc::unsupported: return exit_code::usage_error;
			case errc::protocol: return exit_code::provider_error;
			case errc::io:
			case errc::json:
			case errc::tool_failed:
			case errc::lua_error: return exit_code::verification_failed;
		}

		// Unreachable while the switch is exhaustive, which the compiler enforces
		// because errc has no default case here.
		return exit_code::verification_failed;
	}

	auto exit_code_for_run( const mcode::turn_outcome& outcome, const bool budget_exhausted,
		const bool permission_denied ) -> exit_code {
		// A spent budget is its own exit whatever state ended the run. The loop's
		// plan state reports a spent budget as `failed`, so checking the state
		// first classified it as a provider error -- and in an interactive session
		// the step count is cumulative, so the NEXT turn would exit 4 having done
		// nothing at all.
		if ( budget_exhausted ) {
			return exit_code::budget_exhausted;
		}

		if ( outcome.final_state == mcode::loop_state::failed ) {
			return exit_code::provider_error;
		}

		if ( outcome.final_state != mcode::loop_state::handoff ) {
			return exit_code::success;
		}

		if ( permission_denied ) {
			return exit_code::permission_denied;
		}

		// A handoff is also how a run ends when no verification command is configured, and
		// that is a completion. The loop records a reason only when it gave up -- a failed
		// model call or a guard trip -- which is not a success.
		if ( !outcome.summary_json.empty( ) ) {
			return exit_code::provider_error;
		}

		return exit_code::success;
	}

	auto parse_exec_options( const std::vector< std::string >& arguments )
		-> result< exec_options > {
		auto options = exec_options{ };
		auto index = std::size_t{ 0 };

		const auto next_value = [&]( const std::string_view flag ) -> result< std::string > {
			if ( index + 1 >= arguments.size( ) ) {
				return std::unexpected( fail( errc::config,
					"flag " + std::string{ flag } + " needs a value" ) );
			}

			++index;

			return arguments[ index ];
		};

		for ( ; index < arguments.size( ); ++index ) {
			const auto& argument = arguments[ index ];

			if ( argument == "--json" ) {
				options.json = true;
			} else if ( argument == "--verbose" || argument == "-v" ) {
				options.verbose = true;
			} else if ( argument == "--no-extensions" ) {
				options.no_extensions = true;
			} else if ( argument == "--yolo" ) {
				options.yolo = true;
			} else if ( argument == "--ask" ) {
				options.ask = true;
			} else if ( argument == "--plan" ) {
				options.plan = true;
			} else if ( argument == "--continue" || argument == "-c" ) {
				options.continue_session = true;
			} else if ( argument == "--sessions" ) {
				options.list_sessions = true;
			} else if ( argument == "--worktree" || argument == "-w" ) {
				options.worktree = true;

				// An optional name: only consumed when the next token is not a flag, so
				// `--worktree --json` does not swallow `--json` as a branch name.
				if ( index + 1 < arguments.size( ) && !arguments[ index + 1 ].starts_with( "-" ) ) {
					options.worktree_name = arguments[ ++index ];
				}
			} else if ( argument == "--resume" || argument == "-r" ) {
				auto value = next_value( argument );

				if ( !value ) {
					return std::unexpected( value.error( ) );
				}

				if ( value->empty( ) ) {
					return std::unexpected( fail( errc::config,
						"--resume needs a session id, got an empty value" ) );
				}

				options.resume_session = *value;
			} else if ( argument == "--approval" ) {
				auto value = next_value( argument );

				if ( !value ) {
					return std::unexpected( value.error( ) );
				}

				if ( *value != "never" && *value != "on-request" && *value != "always" ) {
					return std::unexpected( fail( errc::config,
						"--approval needs never, on-request or always, got '" + *value + "'" ) );
				}

				options.approval = *value;
			} else if ( argument == "--add-dir" ) {
				auto value = next_value( argument );

				if ( !value ) {
					return std::unexpected( value.error( ) );
				}

				options.add_dirs.push_back( *value );
			} else if ( argument == "--model" ) {
				auto value = next_value( argument );

				if ( !value ) {
					return std::unexpected( value.error( ) );
				}

				options.model = *value;
			} else if ( argument == "--cwd" ) {
				auto value = next_value( argument );

				if ( !value ) {
					return std::unexpected( value.error( ) );
				}

				options.working_directory = *value;
			} else if ( argument == "--max-steps" ) {
				auto value = next_value( argument );

				if ( !value ) {
					return std::unexpected( value.error( ) );
				}

				// from_chars, and the whole value must be consumed. `stoul` accepted
				// a numeric PREFIX and never threw, so `--max-steps 12abc` silently
				// ran with 12, and a negative value wrapped to a huge step count.
				const auto* first = value->data( );
				const auto* last = value->data( ) + value->size( );
				const auto parsed = std::from_chars( first, last, options.max_steps );

				if ( parsed.ec != std::errc{ } || parsed.ptr != last ) {
					return std::unexpected( fail( errc::config,
						"--max-steps needs a non-negative whole number, got '" + *value + "'" ) );
				}
			} else if ( argument == "--max-budget-usd" ) {
				auto value = next_value( argument );

				if ( !value ) {
					return std::unexpected( value.error( ) );
				}

				if ( !support::parse_double( *value, options.max_budget_usd ) ) {
					return std::unexpected( fail( errc::config,
						"--max-budget-usd needs a number, got '" + *value + "'" ) );
				}
			} else if ( argument.starts_with( '-' ) ) {
				// Unknown flags are collected, never ignored. Silently dropping
				// `--max-step` (a typo) would run with the default budget and the user
				// would never know. A single-dash typo (`-x`) is just as invisible.
				options.unknown_arguments.push_back( argument );
			} else if ( options.prompt.empty( ) ) {
				options.prompt = argument;
			} else {
				// A second bare argument is a usage error, not a silently ignored
				// word: the user probably forgot to quote.
				options.unknown_arguments.push_back( argument );
			}
		}

		// resolved after the loop, so --ask beats --yolo in either order.
		if ( options.ask ) {
			options.yolo = false;
		}

		// resolved after the loop, so an explicit id wins whichever order the two arrive in.
		if ( !options.resume_session.empty( ) ) {
			options.continue_session = false;
		}

		return options;
	}

	auto session_directory( ) -> result< std::filesystem::path > {
		auto state = platform::app_data_path( platform::data_kind::state );

		if ( !state ) {
			return std::unexpected( state.error( ) );
		}

		return *state / std::string{ detail::SESSION_DIRECTORY_NAME };
	}

	auto session_id_for( const std::filesystem::path& workspace_root,
		const std::int64_t started_ms ) -> std::string {
		auto id = detail::workspace_digest( detail::root_for_digest( workspace_root ) );
		id += '-';
		id += detail::padded_stamp( started_ms );

		return id;
	}

	auto list_sessions( const std::filesystem::path& workspace_root )
		-> result< std::vector< session_ref > > {
		auto directory = session_directory( );

		if ( !directory ) {
			return std::unexpected( directory.error( ) );
		}

		auto sessions = detail::scan_sessions( *directory );

		if ( !sessions ) {
			return std::unexpected( sessions.error( ) );
		}

		const auto digest = detail::workspace_digest( detail::root_for_digest( workspace_root ) );

		std::erase_if( *sessions, [ &digest ]( const session_ref& session ) {
			return !session.id.starts_with( digest );
		} );

		return sessions;
	}

	auto resolve_session( const std::filesystem::path& workspace_root,
		const std::string_view id ) -> result< session_ref > {
		auto directory = session_directory( );

		if ( !directory ) {
			return std::unexpected( directory.error( ) );
		}

		if ( !id.empty( ) ) {
			// The id embeds the workspace digest, so a well-formed id still has to belong
			// to this workspace; a foreign one is refused by name rather than reopened.
			if ( !detail::is_session_id( id ) ) {
				return std::unexpected( fail( errc::config,
					"no session named '" + std::string{ id } + "': a session id is 16 hex "
					"digits, a dash, and the start time in milliseconds" ) );
			}

			if ( !id.starts_with( detail::workspace_digest( detail::root_for_digest( workspace_root ) ) ) ) {
				return std::unexpected( fail( errc::config,
					"no session named '" + std::string{ id } + "' for this workspace" ) );
			}

			auto session = session_ref{ };
			session.id = std::string{ id };
			session.path = *directory / detail::session_file_name( id );
			session.started_ms = detail::stamp_of_session_id( id );

			auto error = std::error_code{ };

			if ( !std::filesystem::is_regular_file( session.path, error ) || error ) {
				return std::unexpected( fail( errc::config,
					"no session named '" + session.id + "' in " + directory->string( ) ) );
			}

			const auto size = std::filesystem::file_size( session.path, error );

			session.size_bytes = error ? 0 : size;

			return session;
		}

		auto sessions = list_sessions( workspace_root );

		if ( !sessions ) {
			return std::unexpected( sessions.error( ) );
		}

		if ( sessions->empty( ) ) {
			return std::unexpected( fail( errc::config,
				"no sessions for this workspace yet; run a turn first" ) );
		}

		return sessions->front( );
	}

	auto new_session_ref( const std::filesystem::path& workspace_root )
		-> result< session_ref > {
		auto directory = session_directory( );

		if ( !directory ) {
			return std::unexpected( directory.error( ) );
		}

		auto error = std::error_code{ };
		std::filesystem::create_directories( *directory, error );

		if ( error ) {
			return std::unexpected( fail( errc::io,
				"could not create " + directory->string( ) + ": " + error.message( ) ) );
		}

		auto session = session_ref{ };
		session.started_ms = support::epoch_milliseconds( );
		session.id = session_id_for( workspace_root, session.started_ms );
		session.path = *directory / detail::session_file_name( session.id );

		return session;
	}

	auto open_session_for_run( const exec_options& options,
		const std::filesystem::path& workspace_root ) -> result< session_ref > {
		if ( !options.resume_session.empty( ) ) {
			return resolve_session( workspace_root, options.resume_session );
		}

		if ( options.continue_session ) {
			return resolve_session( workspace_root, { } );
		}

		return new_session_ref( workspace_root );
	}

	auto start_session_log( event_log& log, const session_ref& session, const bool resumed )
		-> std::string {
		// Read before the marker is appended: this is what was on disk, not what this run adds.
		const auto restored_events = resumed ? log.size( ) : std::size_t{ 0 };
		const auto restored_messages = resumed ? restore_transcript( log ).size( )
			: std::size_t{ 0 };

		auto payload = std::string{ "{\"id\":\"" };
		json::append_escaped( payload, session.id );
		payload += "\",\"path\":\"";
		json::append_escaped( payload, session.path.string( ) );
		payload += "\",\"resumed\":";
		payload += resumed ? "true" : "false";
		payload += ",\"restored_events\":";
		payload += std::to_string( restored_events );
		payload += ",\"restored_messages\":";
		payload += std::to_string( restored_messages );
		payload += "}";

		log.append( std::string{ resumed ? detail::SESSION_RESUME_EVENT : detail::SESSION_START_EVENT },
			std::move( payload ) );

		auto report = std::string{ "mcode: session " };
		report += session.id;

		if ( !resumed ) {
			report += " started (";
			report += session.path.string( );
			report += ")\n";

			return report;
		}

		report += " resumed with ";
		report += std::to_string( restored_events );
		report += " recorded events and ";
		report += std::to_string( restored_messages );
		report += restored_messages == 0
			? " messages; this session predates transcript recording, so the turn starts "
				"from an empty history\n"
			: " messages; the transcript is carried into this turn\n";

		return report;
	}

	auto print_sessions( const std::filesystem::path& workspace_root ) -> exit_code {
		auto sessions = list_sessions( workspace_root );
		if ( !sessions ) {
			std::fprintf( stderr, "mcode: %s\n", sessions.error( ).msg.c_str( ) );

			return exit_code::usage_error;
		}

		auto canonical = platform::canonicalize( workspace_root );
		auto header = std::string{ "sessions for " };
		header += canonical ? canonical->string( ) : workspace_root.string( );
		header += ":";

		detail::write_line( header );

		if ( sessions->empty( ) ) {
			detail::write_line( "  (none)" );

			return exit_code::success;
		}

		for ( const auto& session : *sessions ) {
			auto line = std::string{ "  " };
			line += session.id;
			line += "  ";

			if ( session.started_ms > 0 ) {
				line += detail::format_utc( session.started_ms );
			} else {
				line += "unknown start time";
			}

			line += "  ";
			line += std::to_string( session.size_bytes );
			line += " bytes";

			detail::write_line( line );
		}

		auto hint = std::string{ "resume with: mcode exec --resume " };
		hint += sessions->front( ).id;

		detail::write_line( hint );

		return exit_code::success;
	}

	json_stream::json_stream( const bool enabled ) : enabled_( enabled ) { }

	auto json_stream::flush( ) -> void {
		std::fflush( stdout );
	}

	auto json_stream::emit_run_start( const std::string_view prompt ) -> void {
		if ( !enabled_ ) {
			return;
		}

		auto line = std::string{ "{\"v\":1,\"kind\":\"run.start\",\"prompt\":\"" };
		mcode::json::append_escaped( line, prompt );
		line += "\"}";

		detail::write_line( line );
		++lines_;
		flush( );
	}

	auto json_stream::emit_event( const events::event& value ) -> void {
		if ( !enabled_ ) {
			return;
		}

		// One object per line, matching the session log's envelope so a consumer
		// parses both the same way.
		auto line = std::string{ "{\"v\":1,\"seq\":" };
		line += std::to_string( value.sequence );
		line += ",\"ts\":";
		line += std::to_string( value.timestamp_ms );
		line += ",\"kind\":\"";
		mcode::json::append_escaped( line, events::to_string( value.type ) );
		line += "\"";
		line += ",\"payload\":";
		line += value.payload_json.empty( ) ? "{}" : value.payload_json;
		line += "}";

		detail::write_line( line );
		++lines_;
		flush( );
	}

	auto json_stream::emit_run_end( const exit_code code, const std::string_view summary ) -> void {
		if ( !enabled_ ) {
			// Even when JSON is off, record that the run concluded, so the
			// destructor's truncated-stream warning stays meaningful.
			run_end_emitted_ = true;

			return;
		}

		// Emitted at most once. A consumer treats run.end as the signal that the run
		// finished rather than was cut off, so a second one would be ambiguous.
		if ( run_end_emitted_ ) {
			return;
		}

		auto line = std::string{ "{\"v\":1,\"kind\":\"run.end\",\"exit_code\":" };
		line += std::to_string( to_int( code ) );
		line += ",\"summary\":\"";
		mcode::json::append_escaped( line, summary );
		line += "\"}";

		detail::write_line( line );
		++lines_;
		run_end_emitted_ = true;
		flush( );
	}

	json_stream::~json_stream( ) {
		if ( enabled_ && !run_end_emitted_ ) {
			// A truncated stream is a bug in the caller, not a normal exit. Saying so
			// on stderr is better than a consumer waiting for a run.end that will
			// never arrive.
			std::fputs( "mcode: json stream closed without run.end\n", stderr );
		}
	}

	auto usage_text( const std::string_view program ) -> std::string {
		auto out = std::string{ "usage: " };
		out += program;
		out += " exec [options] [prompt]\n\n";
		out += "options:\n";
		out += "  --json                 emit one JSON object per line on stdout\n";
		out += "  --model <name>         model to use\n";
		out += "  --cwd <path>           working directory\n";
		out += "  --max-steps <n>        stop after n steps\n";
		out += "  --max-budget-usd <n>   stop after spending n USD\n";
		out += "  --approval <mode>      never | on-request | always (default never)\n";
		out += "  --ask                  restore prompting for every mutating action\n";
		out += "  --plan                 read-only: refuse edits and commands\n";
		out += "  --continue, -c         reopen the newest session for this workspace\n";
		out += "  --resume <id>, -r <id> reopen the session with this id (see --sessions)\n";
		out += "  --sessions             list this workspace's sessions and exit\n";
		out += "  --worktree [name], -w  run in a fresh git worktree under .mcode/worktrees/\n";
		out += "  --add-dir <path>       add an extra workspace root for this run\n";
		out += "  --no-extensions        disable every extension\n";
		out += "  --yolo                 skip approval prompts; the hard-deny floor and\n";
		out += "                         permissions.deny still apply; spawned commands run\n";
		out += "                         under the OS sandbox where the platform supports it\n";
		out += "  -v, --verbose          more diagnostics on stderr\n";
		out += "\nexit codes:\n";
		out += "  0 completed   1 verification failed   2 usage error\n";
		out += "  3 budget      4 provider error        5 permission denied\n";
		out += "  130 interrupted\n";

		return out;
	}

}
