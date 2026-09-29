#include "mcode/tools/exec_tools.hxx"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "mcode/perm/argv.hxx"
#include "mcode/proc/process.hxx"
#include "mcode/tools/errors.hxx"
#include "mcode/tools/truncate.hxx"
#include "mcode/support/text.hxx"

#include <charconv>

namespace mcode::tools {

	namespace {

		inline constexpr std::int64_t DEFAULT_BASH_TIMEOUT_MS = 60'000;
		inline constexpr std::int64_t MAX_BASH_TIMEOUT_MS = 600'000;

		// The registry entries tool_search scores. Kept as a request struct so the
		// scorer stays a pure function over data.
		struct scored_tool {
			std::string name;
			std::string description;
			double score = 0.0;
		};

		// BM25-class scoring over name and description tokens: exact name-token
		// hits dominate, description hits and prefixes add signal. The corpus is a
		// handful of tools, so the full IDF/length machinery would be bookkeeping
		// without effect.
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
				if ( character == '_' || character == ' ' || character == '-' || character == '.' ) {
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
				"the approval gate matches single commands only; split compound commands (&& | ; $() backticks) into separate calls",
				false );
		}

		auto request = perm::permission_request{ };
		request.tool_name = "bash";
		request.klass = tool_class::exec;
		request.resource = perm::canonical_argv( *tokens );

		const auto decision = context.permissions->decide( request );

		if ( decision != perm::permission_decision::allow ) {
			const auto& verdict = context.permissions->last_verdict( );
			const auto& program = tokens->front( );

			auto message = std::string{ "exec denied by the permission engine: " + program };

			if ( !verdict.reason.empty( ) ) {
				message += " (" + verdict.reason + ")";
			}

			return error_result( message,
				verdict.matched.scope == "floor"
					? "this action is on the hard-deny floor; no flag or config overrides it"
					: "the command prompts or is denied; run it interactively to approve it, or add an allow rule",
				false );
		}

		// Execute the argv the policy judged, not the raw string: a shell would
		// re-split, redirect or expand text the gate never approved. The shell
		// itself is an explicitly denied runner, so nothing here goes through one.
		auto options = process_options{ };
		options.working_directory = context.space->root( ).string( );
		options.timeout = std::chrono::milliseconds{ timeout_ms };
		options.scrub_environment = true;

		const auto program_path = find_executable( tokens->front( ) );

		if ( !program_path ) {
			return error_result( "cannot resolve " + tokens->front( ) + " on PATH",
				"the program was approved but not found; check the spelling and that it is installed",
				false );
		}

		options.executable = *program_path;
		options.args.assign( tokens->begin( ) + 1, tokens->end( ) );

		auto outcome = run_process( options );

		if ( !outcome ) {
			return error_result( "failed to spawn: " + outcome.error( ).msg,
				"check the command exists on PATH; the spawn itself failed, not the command",
				false );
		}

		auto out = std::string{ "{\"ok\":true,\"exit_code\":" };
		out += std::to_string( outcome->exit_code );
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
			out += ",\"hint\":\"the command failed with exit code " +
				std::to_string( outcome->exit_code ) + "; read stderr above before retrying -- exec calls are never auto-retried\"";
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
				"no terminal is attached; decide from available evidence, or re-run mcode interactively to answer this question",
				false );
		}

		const auto answer = prompt_and_read( *question, options );

		if ( answer.empty( ) ) {
			return error_result( "no answer was given",
				"the user closed or emptied the prompt; proceed without the answer or ask again with sharper options",
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
				"pass words to match against tool names and descriptions, e.g. \"git commit\"", false );
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
