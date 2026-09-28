#include "mcode/cli/exec.hxx"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "mcode/support/json.hxx"

namespace mcode::cli {

	namespace {

		auto escape_for_json( const std::string_view text ) -> std::string {
			auto out = std::string{ };
			out.reserve( text.size( ) + 8 );

			for ( const auto character : text ) {
				switch ( character ) {
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default:
						if ( static_cast< unsigned char >( character ) < 0x20 ) {
							char buffer[ 8 ]{ };
							std::snprintf( buffer, sizeof( buffer ), "\\u%04x",
								static_cast< unsigned char >( character ) );
							out += buffer;
						} else {
							out.push_back( character );
						}
				}
			}

			return out;
		}

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

	auto parse_exec_options( const std::vector< std::string >& arguments ) -> result< exec_options > {
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

				try {
					options.max_steps = static_cast< std::uint32_t >( std::stoul( *value ) );
				} catch ( ... ) {
					return std::unexpected( fail( errc::config,
						"--max-steps needs a number, got '" + *value + "'" ) );
				}
			} else if ( argument == "--max-budget-usd" ) {
				auto value = next_value( argument );

				if ( !value ) {
					return std::unexpected( value.error( ) );
				}

				try {
					options.max_budget_usd = std::stod( *value );
				} catch ( ... ) {
					return std::unexpected( fail( errc::config,
						"--max-budget-usd needs a number, got '" + *value + "'" ) );
				}
			} else if ( argument.starts_with( "--" ) ) {
				// Unknown flags are collected, never ignored. Silently dropping
				// `--max-step` (a typo) would run with the default budget and the user
				// would never know.
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
		line += escape_for_json( prompt );
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
		line += ",\"kind\":\"" + escape_for_json( events::to_string( value.type ) ) + "\"";
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
		line += ",\"summary\":\"" + escape_for_json( summary ) + "\"}";

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
		out += "  --no-extensions        disable every extension\n";
		out += "  --yolo                 skip approval prompts (still sandboxed)\n";
		out += "  -v, --verbose          more diagnostics on stderr\n";
		out += "\nexit codes:\n";
		out += "  0 completed   1 verification failed   2 usage error\n";
		out += "  3 budget      4 provider error        5 permission denied\n";
		out += "  130 interrupted\n";

		return out;
	}

}
