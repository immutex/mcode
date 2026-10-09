#include "mcode/tools/exec_tools.hxx"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "mcode/perm/argv.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/proc/process.hxx"
#include "mcode/tools/errors.hxx"
#include "mcode/tools/truncate.hxx"
#include "mcode/support/text.hxx"

#include <charconv>

namespace mcode::tools {

	namespace {

		inline constexpr std::int64_t DEFAULT_BASH_TIMEOUT_MS = 60'000;
		inline constexpr std::int64_t MAX_BASH_TIMEOUT_MS = 600'000;

		// A negative exit code is never a program's own return value: it is how
		// the platform reports that the process did not exit normally. Naming the
		// common ones turns "exit code -1073741502" into something a caller can
		// act on, which matters because the natural response to a crash is to
		// retry the same command rather than to stop using that binary.
		[[nodiscard]] auto decode_crash_code( const int exit_code ) -> std::string {
			if ( exit_code >= 0 ) {
				return { };
			}

			// Windows reports an NTSTATUS; POSIX reports a signal number.
		#if defined( _WIN32 )
			const auto status = static_cast< std::uint32_t >( exit_code );

			switch ( status ) {
				case 0xC0000142u:
					return "the process could not start (0xC0000142, DLL init failed). This "
						"binary is unusable in this environment -- MSYS/Cygwin tools such as "
						"Git's grep, rm and sed fail under the sandbox because they create a "
						"named object it denies. Use a native tool or the dedicated grep, "
						"glob and read tools instead; do not retry this one";
				case 0xC0000135u:
					return "the process could not start (0xC0000135, a required DLL was not "
						"found). Run a native tool or the dedicated file tools instead";
				case 0xC0000005u:
					return "the process crashed with an access violation (0xC0000005). This is "
						"a fault in the program, not in the command; do not retry it";
				case 0xC0000409u:
					return "the process aborted with a stack buffer overrun (0xC0000409); do "
						"not retry it";
				default: {
					auto hex = std::array< char, 9 >{ };
					std::snprintf( hex.data( ), hex.size( ), "%08X", status );

					return std::string{ "the process did not exit normally (NTSTATUS 0x" } +
						hex.data( ) + "); it likely crashed or was killed";
				}
			}
		#else
			return "the process was killed by signal " + std::to_string( -exit_code ) +
				"; it did not exit on its own";
		#endif
		}

		struct scored_tool {
			std::string name;
			std::string description;
			double score = 0.0;
		};

		[[nodiscard]] auto lower_ascii( std::string_view text ) -> std::string {
			auto out = std::string{ text };

			for ( auto& character : out ) {
				if ( character >= 'A' && character <= 'Z' ) {
					character = static_cast< char >( character - 'A' + 'a' );
				}
			}

			return out;
		}

		[[nodiscard]] auto tokenize( const std::string_view text ) -> std::vector< std::string > {
			auto tokens = std::vector< std::string >{ };
			auto current = std::string{ };

			for ( const auto character : text ) {
				if ( character == '_' || character == ' ' || character == '-' ||
					character == '.' ) {
					if ( !current.empty( ) ) {
						tokens.push_back( std::move( current ) );
						current.clear( );
					}

					continue;
				}

				current += character;
			}

			if ( !current.empty( ) ) {
				tokens.push_back( std::move( current ) );
			}

			return tokens;
		}

		[[nodiscard]] auto score_tool( const tool_def& definition,
			const std::vector< std::string >& query_tokens ) -> double {
			auto name_tokens = tokenize( lower_ascii( definition.name ) );
			auto description_tokens = tokenize( lower_ascii( definition.description ) );

			auto score = 0.0;

			for ( const auto& query : query_tokens ) {
				for ( const auto& token : name_tokens ) {
					if ( token == query ) {
						score += 3.0;
					} else if ( token.starts_with( query ) || query.starts_with( token ) ) {
						score += 1.0;
					}
				}

				for ( const auto& token : description_tokens ) {
					if ( token == query ) {
						score += 1.0;
					}
				}
			}

			return score;
		}

		[[nodiscard]] auto prompt_and_read( const std::string& question,
			const std::vector< std::string >& options ) -> std::string {
			std::fputs( "\n[agent asks] ", stdout );
			std::fputs( question.c_str( ), stdout );
			std::fputs( "\n", stdout );

			if ( !options.empty( ) ) {
				for ( auto index = std::size_t{ 0 }; index < options.size( ); ++index ) {
					std::printf( "  %zu) %s\n", index + 1, options[ index ].c_str( ) );
				}

				std::fputs( "choose a number or type your own answer: ", stdout );
			} else {
				std::fputs( "your answer: ", stdout );
			}

			std::fflush( stdout );

			auto line = std::string{ };

			if ( !std::getline( std::cin, line ) ) {
				return { };
			}

			if ( !options.empty( ) ) {
				auto number = std::int64_t{ 0 };
				const auto* begin = line.c_str( );
				const auto* end = begin + line.size( );
				const auto parsed = std::from_chars( begin, end, number );

				if ( parsed.ec == std::errc{ } && number >= 1 &&
					static_cast< std::size_t >( number ) <= options.size( ) ) {
					return options[ static_cast< std::size_t >( number - 1 ) ];
				}
			}

			return line;
		}

	}

	auto handle_bash( const tool_args& args, tool_context& context ) -> result< std::string > {
		const auto command = args.string_field( "command" );

		if ( !command || command->empty( ) ) {
			return error_result( "missing required argument: command",
				"pass a single shell command to run in the workspace root", false );
		}

		auto timeout_ms = DEFAULT_BASH_TIMEOUT_MS;

		if ( const auto requested = args.int_field( "timeout_ms" ) ) {
			if ( *requested < 1 ) {
				return error_result( "timeout_ms must be positive",
					"pass timeout_ms >= 1; the default is 60000", false );
			}

			timeout_ms = std::min( *requested, MAX_BASH_TIMEOUT_MS );
		}

		const auto tokens = perm::parse_command_line( *command );

		if ( !tokens ) {
			return error_result(
				"refusing unparsable or compound command: " + *command,
				"the approval gate matches single commands only; "
				"split compound commands (&& | ; $() backticks) into separate calls",
				false );
		}

		// the permission decision happens in the loop, before any handler runs -- never here

		// run the argv the gate judged: a shell would re-split or expand text it never approved
		auto options = process_options{ };
		options.working_directory = context.space->root( ).string( );
		options.timeout = std::chrono::milliseconds{ timeout_ms };
		options.scrub_environment = true;

		// the OS boundary is what makes auto-allowed exec safe; the engine is a gate, not isolation
		auto profile = platform::sandbox_profile{ };
		profile.read_paths.push_back( context.space->root( ) );
		profile.write_paths.push_back( context.space->root( ) );

		// the root write grant would otherwise cover these
		profile.deny_paths.push_back( context.space->root( ) / ".git" );
		profile.deny_paths.push_back( context.space->root( ) / ".mcode" );

		if ( const auto temp = platform::temp_directory( ) ) {
			profile.read_paths.push_back( *temp );
			profile.write_paths.push_back( *temp );
		}

		profile.allow_network = false;
		options.sandbox = &profile;

		const auto program_path = find_executable( tokens->front( ) );

		if ( !program_path ) {
			return error_result( "cannot resolve " + tokens->front( ) + " on PATH",
				"the program was approved but not found; "
				"check the spelling and that it is installed",
				false );
		}

		options.executable = *program_path;
		options.args.assign( tokens->begin( ) + 1, tokens->end( ) );

		auto outcome = run_process( options );

		if ( !outcome ) {
			return error_result( "failed to spawn: " + outcome.error( ).msg,
				"check the command exists on PATH; the spawn itself failed, not the command -- "
				"if the message names the sandbox, the platform refused the profile and the run "
				"proceeds with the permission engine alone",
				false );
		}

		auto out = std::string{ "{\"ok\":true,\"exit_code\":" };
		out += std::to_string( outcome->exit_code );

		// `ok` means the tool ran, which is not what the caller wants to know: a
		// crashed command reported `ok:true` and the model read it as success.
		// This states the outcome in the field the reader is actually looking for.
		out += ",\"succeeded\":";
		out += ( outcome->exit_code == 0 && !outcome->timed_out ) ? "true" : "false";

		out += ",\"timed_out\":";

		out += outcome->timed_out ? "true" : "false";
		out += ",\"output_truncated\":";

		out += outcome->output_truncated ? "true" : "false";
		out += ",\"stdout\":\"";
		json::append_escaped( out, text::sanitize_utf8( outcome->stdout_text ) );
		out += "\",\"stderr\":\"";
		json::append_escaped( out, text::sanitize_utf8( outcome->stderr_text ) );
		out += "\"";

		if ( outcome->timed_out ) {
			out += ",\"hint\":\"the process was killed at the " + std::to_string( timeout_ms ) +
				" ms timeout; re-run with a larger timeout_ms if the command needs longer\"";
		} else if ( outcome->exit_code != 0 ) {
			out += ",\"hint\":\"";

			// A negative code is not a program's own return value. On Windows it
			// is an NTSTATUS, and the common ones mean the process could not
			// START -- which is a different problem from a command that ran and
			// failed, and the one a model most often misreads. Measured: an MSYS
			// `grep`/`rm` under the sandbox died with 0xC0000142, and the model
			// retried the same shape of command rather than switching tools.
			if ( const auto decoded = decode_crash_code( outcome->exit_code );
				!decoded.empty( ) ) {
				out += decoded;
			} else {
				out += "the command failed with exit code " +
					std::to_string( outcome->exit_code ) +
					"; read stderr above before retrying -- exec calls are never auto-retried";
			}

			out += "\"";
		}

		if ( outcome->output_truncated ) {
			out += ",\"output_note\":\"output exceeded the capture cap and was cut mid-stream\"";
		}

		out += "}";

		return truncate_result( out, context.run_id, *context.space, "bash" );
	}

	auto handle_ask_user( const tool_args& args, tool_context& context ) -> result< std::string > {
		const auto question = args.string_field( "question" );

		if ( !question || question->empty( ) ) {
			return error_result( "missing required argument: question",
				"pass the question to put to the user, self-contained", false );
		}

		auto options = std::vector< std::string >{ };

		if ( const auto requested = args.string_array_field( "options" ) ) {
			options = *requested;
		}

		if ( context.headless ) {
			return error_result( "cannot ask the user in headless mode: " + *question,
				"no terminal is attached; decide from available evidence, "
				"or re-run mcode interactively to answer this question",
				false );
		}

		const auto answer = prompt_and_read( *question, options );

		if ( answer.empty( ) ) {
			return error_result( "no answer was given",
				"the user closed or emptied the prompt; "
				"proceed without the answer or ask again with sharper options",
				false );
		}

		auto out = std::string{ "{\"ok\":true,\"answer\":\"" };
		json::append_escaped( out, answer );
		out += "\",\"question\":\"";
		json::append_escaped( out, *question );
		out += "\"}";

		return out;
	}

	auto handle_tool_search( const tool_args& args, tool_context& context,
		const tool_registry& registry ) -> result< std::string > {
		const auto query = args.string_field( "query" );

		if ( !query || query->empty( ) ) {
			return error_result( "missing required argument: query",
				"pass words to match against tool names and descriptions, "
				"e.g. \"git commit\"", false );
		}

		const auto expand = args.bool_field( "expand" ).value_or( false );
		const auto query_tokens = tokenize( lower_ascii( *query ) );

		auto scored = std::vector< scored_tool >{ };

		for ( const auto* definition : registry.all( ) ) {
			const auto score = score_tool( *definition, query_tokens );

			if ( score > 0.0 ) {
				scored.push_back( scored_tool{ definition->name, definition->description, score } );
			}
		}

		std::sort( scored.begin( ), scored.end( ),
			[ ]( const scored_tool& left, const scored_tool& right ) {
				if ( left.score != right.score ) {
					return left.score > right.score;
				}

				return left.name < right.name;
			} );

		auto out = std::string{ "{\"ok\":true,\"tools\":[" };
		auto first = true;

		for ( const auto& entry : scored ) {
			if ( !first ) {
				out += ',';
			}

			first = false;
			out += "{\"name\":\"";
			json::append_escaped( out, entry.name );
			out += "\",\"description\":\"";
			json::append_escaped( out, entry.description );
			out += "\"";

			if ( expand ) {
				const auto* definition = registry.find( entry.name );

				if ( definition != nullptr && !definition->schema_json.empty( ) ) {
					out += ",\"schema\":";
					out += definition->schema_json;
				}
			}

			out += "}";
		}

		out += "],\"count\":" + std::to_string( scored.size( ) );

		if ( !expand && !scored.empty( ) ) {
			out += ",\"hint\":\"call again with expand=true to load the full schema of a match\"";
		}

		out += "}";

		return truncate_result( out, context.run_id, *context.space, "tool_search" );
	}

}
