#include "mcode/cli/exec.hxx"

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

	namespace {

		// The state directory's subdirectory, and the suffix every session file carries.
		inline constexpr std::string_view SESSION_DIRECTORY_NAME = "sessions";
		inline constexpr std::string_view SESSION_FILE_SUFFIX = ".jsonl";

		// `git worktree add` copies the tree, so it is slower than a status call but bounded.
		inline constexpr std::int64_t WORKTREE_COMMAND_TIMEOUT_MS = 120'000;

		// The label used when the caller names no worktree.
		inline constexpr std::string_view DEFAULT_WORKTREE_NAME = "run";

		// workspace digest, '-', epoch milliseconds padded to 13 digits.
		inline constexpr std::size_t SESSION_DIGEST_LENGTH = 16;
		inline constexpr std::size_t SESSION_STAMP_LENGTH = 13;
		inline constexpr std::size_t SESSION_ID_LENGTH =
			SESSION_DIGEST_LENGTH + 1 + SESSION_STAMP_LENGTH;

		inline constexpr std::string_view SESSION_START_EVENT = "session.start";
		inline constexpr std::string_view SESSION_RESUME_EVENT = "session.resume";

		auto write_line( const std::string_view text ) -> void {
			std::fwrite( text.data( ), 1, text.size( ), stdout );
			std::fputc( '\n', stdout );
		}

		// u8string, not string: a non-ASCII workspace path must not be narrowed to the
		// system code page before it is hashed.
		auto to_utf8( const std::filesystem::path& path ) -> std::string {
			const auto text = path.generic_u8string( );

			return std::string{ reinterpret_cast< const char* >( text.data( ) ), text.size( ) };
		}

		// FNV-1a over the canonical root's UTF-8 bytes, so two workspaces cannot collide
		// on one session id.
		auto workspace_digest( const std::filesystem::path& canonical_root ) -> std::string {
			return mcode::hash_bytes( to_utf8( canonical_root ) );
		}

		// The canonical root when it resolves, the normalized absolute path otherwise. Every
		// entry point derives the digest through here, so `list`, `resolve` and the run that
		// writes the file can never disagree about which workspace a session belongs to.
		auto root_for_digest( const std::filesystem::path& workspace_root )
			-> std::filesystem::path {
			auto canonical = platform::canonicalize( workspace_root );

			if ( canonical ) {
				return *canonical;
			}

			auto error = std::error_code{ };
			auto absolute = std::filesystem::absolute( workspace_root, error );

			if ( error ) {
				return workspace_root.lexically_normal( );
			}

			return absolute.lexically_normal( );
		}

		auto session_file_name( const std::string_view id ) -> std::string {
			return std::string{ id } + std::string{ SESSION_FILE_SUFFIX };
		}

		// Zero-padded, so a lexical sort of the names is a chronological sort.
		auto padded_stamp( const std::int64_t start_ms ) -> std::string {
			auto text = std::to_string( start_ms );

			if ( text.size( ) < SESSION_STAMP_LENGTH ) {
				text.insert( 0, SESSION_STAMP_LENGTH - text.size( ), '0' );
			}

			return text;
		}

		// An id is a hex digest, a dash, and a decimal stamp -- nothing else. A user-supplied
		// id is refused before it is joined to a path, so `../` cannot leave the directory.
		auto is_session_id( const std::string_view id ) -> bool {
			if ( id.size( ) != SESSION_ID_LENGTH || id[ SESSION_DIGEST_LENGTH ] != '-' ) {
				return false;
			}

			for ( auto index = std::size_t{ 0 }; index < id.size( ); ++index ) {
				if ( index == SESSION_DIGEST_LENGTH ) {
					continue;
				}

				const auto character = id[ index ];
				const auto is_hex = ( character >= '0' && character <= '9' )
					|| ( character >= 'a' && character <= 'f' );

				if ( !is_hex ) {
					return false;
				}
			}

			return true;
		}

		// The epoch-milliseconds stamp the id ends with, or 0 when it is not one.
		auto stamp_of_session_id( const std::string_view id ) -> std::int64_t {
			const auto digits = id.substr( SESSION_DIGEST_LENGTH + 1 );
			auto value = std::int64_t{ 0 };
			const auto parsed = std::from_chars( digits.data( ), digits.data( ) + digits.size( ),
				value );

			if ( parsed.ec != std::errc{ } ) {
				return 0;
			}

			return value;
		}

		// Every well-formed session file in `directory`, newest first. The workspace digest
		// filter belongs to the caller: resolving an id has to see a session from another
		// workspace in order to name it, rather than report that no such session exists.
		auto scan_sessions( const std::filesystem::path& directory )
			-> result< std::vector< session_ref > > {
			auto out = std::vector< session_ref >{ };
			auto error = std::error_code{ };

			// A workspace with no session yet is the normal first run, not a failure.
			if ( !std::filesystem::is_directory( directory, error ) || error ) {
				return out;
			}

			auto iterator = std::filesystem::directory_iterator{ directory, error };

			if ( error ) {
				return std::unexpected( fail( errc::io,
					"could not read " + directory.string( ) + ": " + error.message( ) ) );
			}

			for ( const auto& entry : iterator ) {
				// A fresh code per call: a stale one would skip every later entry.
				auto entry_error = std::error_code{ };

				if ( !entry.is_regular_file( entry_error ) || entry_error ) {
					continue;
				}

				const auto name = to_utf8( entry.path( ).filename( ) );

				if ( !name.ends_with( SESSION_FILE_SUFFIX ) ) {
					continue;
				}

				const auto id = name.substr( 0, name.size( ) - SESSION_FILE_SUFFIX.size( ) );

				if ( !is_session_id( id ) ) {
					continue;
				}

				auto session = session_ref{ };
				session.path = entry.path( );
				session.id = id;
				session.started_ms = stamp_of_session_id( id );

				const auto size = std::filesystem::file_size( session.path, entry_error );

				session.size_bytes = entry_error ? 0 : size;

				out.push_back( std::move( session ) );
			}

			// Newest first: the id carries the start time, so the ordering is deterministic
			// even when two runs land in the same millisecond.
			std::sort( out.begin( ), out.end( ),
				[]( const session_ref& left, const session_ref& right ) {
					if ( left.started_ms != right.started_ms ) {
						return left.started_ms > right.started_ms;
					}

					return left.id > right.id;
				} );

			return out;
		}

		auto format_utc( const std::int64_t milliseconds ) -> std::string {
			const auto seconds = static_cast< std::time_t >( milliseconds / 1000 );
			auto utc = std::tm{ };

		#if defined( _WIN32 )
			gmtime_s( &utc, &seconds );
		#else
			gmtime_r( &seconds, &utc );
		#endif

			auto buffer = std::array< char, 32 >{ };
			std::strftime( buffer.data( ), buffer.size( ), "%Y-%m-%dT%H:%M:%SZ", &utc );

			return buffer.data( );
		}

	}

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
		if ( outcome.final_state == mcode::loop_state::failed ) {
			return exit_code::provider_error;
		}

		if ( outcome.final_state != mcode::loop_state::handoff ) {
			return exit_code::success;
		}

		// Budget before denial: a run that ran out of steps and was denied is a budget exit.
		if ( budget_exhausted ) {
			return exit_code::budget_exhausted;
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

		return *state / std::string{ SESSION_DIRECTORY_NAME };
	}

	auto session_id_for( const std::filesystem::path& workspace_root,
		const std::int64_t started_ms ) -> std::string {
		auto id = workspace_digest( root_for_digest( workspace_root ) );
		id += '-';
		id += padded_stamp( started_ms );

		return id;
	}

	auto list_sessions( const std::filesystem::path& workspace_root )
		-> result< std::vector< session_ref > > {
		auto directory = session_directory( );

		if ( !directory ) {
			return std::unexpected( directory.error( ) );
		}

		auto sessions = scan_sessions( *directory );

		if ( !sessions ) {
			return std::unexpected( sessions.error( ) );
		}

		const auto digest = workspace_digest( root_for_digest( workspace_root ) );

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
			if ( !is_session_id( id ) ) {
				return std::unexpected( fail( errc::config,
					"no session named '" + std::string{ id } + "': a session id is 16 hex "
					"digits, a dash, and the start time in milliseconds" ) );
			}

			if ( !id.starts_with( workspace_digest( root_for_digest( workspace_root ) ) ) ) {
				return std::unexpected( fail( errc::config,
					"no session named '" + std::string{ id } + "' for this workspace" ) );
			}

			auto session = session_ref{ };
			session.id = std::string{ id };
			session.path = *directory / session_file_name( id );
			session.started_ms = stamp_of_session_id( id );

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

	auto open_session_for_run( const exec_options& options,
		const std::filesystem::path& workspace_root ) -> result< session_ref > {
		if ( !options.resume_session.empty( ) ) {
			return resolve_session( workspace_root, options.resume_session );
		}

		if ( options.continue_session ) {
			return resolve_session( workspace_root, { } );
		}

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
		session.path = *directory / session_file_name( session.id );

		return session;
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

		log.append( std::string{ resumed ? SESSION_RESUME_EVENT : SESSION_START_EVENT },
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

	// Rebuilds the conversation from a session log. Only the events that carry a
	// message or a tool output contribute; the activity events (`tool.call`,
	// `run.end`, `session.*`) are diagnostics and are not part of the transcript.
	//
	// A tool output is attached to the assistant message that requested it, which
	// is where the model expects it: the loop records the assistant message
	// before dispatch, so the outputs follow it in the log.
	[[nodiscard]] auto restore_transcript( const event_log& log )
		-> std::vector< model::message > {
		auto history = std::vector< model::message >{ };

		for ( const auto& recorded : log.events( ) ) {
			if ( recorded.kind == mcode::agent::MESSAGE_USER_EVENT
				|| recorded.kind == mcode::agent::MESSAGE_ASSISTANT_EVENT ) {
				auto message = mcode::agent::message_from_json( recorded.payload_json );

				if ( message ) {
					history.push_back( std::move( *message ) );
				}

				continue;
			}

			if ( recorded.kind != mcode::agent::TOOL_RESULT_EVENT ) {
				continue;
			}

			auto document = json::document::parse( recorded.payload_json );

			if ( !document ) {
				continue;
			}

			auto content = document->get_string( "content" );

			if ( !content ) {
				continue;
			}

			// The result rides on the last assistant message, as a tool block
			// carrying the id the call used.
			if ( history.empty( ) ) {
				continue;
			}

			auto& last = history.back( );

			if ( last.speaker != model::role::assistant ) {
				continue;
			}

			for ( auto& block : last.blocks ) {
				if ( block.kind != model::block_kind::tool_call ) {
					continue;
				}

				auto result = model::block{ };
				result.kind = model::block_kind::tool_result;
				result.tool_call_id = block.tool_call_id;
				result.result_json = *content;
				last.blocks.push_back( std::move( result ) );

				break;
			}
		}

		return history;
	}

	// Creates a git worktree beside the checkout and returns its path. The worktree is the
	// blast-radius limit: an unattended run cannot touch the user's working tree, and the
	// rollback is `git worktree remove` rather than a snapshot store.
	auto create_worktree( const std::filesystem::path& workspace_root,
		const std::string_view name ) -> result< std::filesystem::path > {
		auto git = mcode::find_executable( "git" );

		if ( !git ) {
			return std::unexpected( fail( errc::config,
				"--worktree needs git on PATH: " + git.error( ).msg ) );
		}

		// The workspace root must be the repository root itself. `git worktree add` walks up
		// to an enclosing repository, so a directory that merely sits inside one would
		// silently check out the WRONG project - and in a plain directory under a repository
		// it would appear to succeed. Asking for the top level and comparing refuses both.
		auto top = mcode::process_options{ };
		top.executable = *git;
		top.args = { "rev-parse", "--show-toplevel" };
		top.working_directory = workspace_root.string( );
		top.environment = mcode::minimal_environment( );
		top.timeout = std::chrono::milliseconds{ WORKTREE_COMMAND_TIMEOUT_MS };

		auto probe = mcode::run_process( top );

		if ( !probe || probe->exit_code != 0 ) {
			return std::unexpected( fail( errc::config,
				"--worktree needs the workspace to be a git repository root, but " +
				workspace_root.string( ) + " is not inside one" ) );
		}

		auto reported = probe->stdout_text;

		while ( !reported.empty( ) && ( reported.back( ) == '\n' || reported.back( ) == '\r' ) ) {
			reported.pop_back( );
		}

		const auto canonical_root = platform::canonicalize( workspace_root );
		const auto canonical_top = platform::canonicalize( std::filesystem::path{ reported } );

		if ( !canonical_root || !canonical_top || *canonical_root != *canonical_top ) {
			return std::unexpected( fail( errc::config,
				"--worktree needs the workspace to be the repository root; the enclosing "
				"repository is " + reported + ", so a worktree of it would be the wrong "
				"project" ) );
		}

		// A short unique suffix keeps two runs from colliding without requiring the caller
		// to name each one.
		auto label = std::string{ name };

		if ( label.empty( ) ) {
			label = std::string{ DEFAULT_WORKTREE_NAME };
		}

		label += "-";
		label += std::to_string( mcode::support::epoch_milliseconds( ) );

		const auto target = workspace_root / ".mcode" / "worktrees" / label;

		auto options = mcode::process_options{ };
		options.executable = *git;
		options.args = { "worktree", "add", "--detach", target.string( ) };
		options.working_directory = workspace_root.string( );
		options.environment = mcode::minimal_environment( );
		options.timeout = std::chrono::milliseconds{ WORKTREE_COMMAND_TIMEOUT_MS };

		auto ran = mcode::run_process( options );

		if ( !ran ) {
			return std::unexpected( fail( errc::config,
				"could not create a worktree: " + ran.error( ).msg ) );
		}

		// A non-zero exit means git refused, and its stderr says why. Surfacing it verbatim
		// is the only way the caller learns the directory is not a repository.
		if ( ran->exit_code != 0 ) {
			auto detail = ran->stderr_text;

			while ( !detail.empty( ) && ( detail.back( ) == '\n' || detail.back( ) == '\r' ) ) {
				detail.pop_back( );
			}

			return std::unexpected( fail( errc::config,
				"git worktree add failed: " + ( detail.empty( ) ? ran->stdout_text : detail ) ) );
		}

		return target;
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

		write_line( header );

		if ( sessions->empty( ) ) {
			write_line( "  (none)" );

			return exit_code::success;
		}

		for ( const auto& session : *sessions ) {
			auto line = std::string{ "  " };
			line += session.id;
			line += "  ";

			if ( session.started_ms > 0 ) {
				line += format_utc( session.started_ms );
			} else {
				line += "unknown start time";
			}

			line += "  ";
			line += std::to_string( session.size_bytes );
			line += " bytes";

			write_line( line );
		}

		auto hint = std::string{ "resume with: mcode exec --resume " };
		hint += sessions->front( ).id;

		write_line( hint );

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

		write_line( line );
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

		write_line( line );
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

		write_line( line );
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
