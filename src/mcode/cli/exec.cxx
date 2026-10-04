#include "mcode/cli/exec.hxx"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "mcode/agent/loop.hxx"
#include "mcode/support/json.hxx"
#include "mcode/support/parse.hxx"

namespace mcode::cli {

	namespace {

		auto write_line( const std::string_view text ) -> void {
			std::fwrite( text.data( ), 1, text.size( ), stdout );
			std::fputc( '\n', stdout );
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

		return options;
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
		out += "  --approval <mode>      never | on-request | always (default on-request)\n";
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
