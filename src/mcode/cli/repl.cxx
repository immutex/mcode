#include "mcode/cli/repl.hxx"

#include <cstdio>
#include <iostream>
#include <string>
#include <utility>

namespace mcode::cli {

	auto session::run_turn( const std::string_view task ) -> cli::exit_code {
		const auto outcome = loop_->run( task );

		if ( !outcome ) {
			return cli::exit_code_for( outcome.error( ).code );
		}

		auto code = cli::exit_code::success;

		if ( outcome->final_state == mcode::loop_state::failed ) {
			code = cli::exit_code::provider_error;
		} else if ( outcome->final_state == mcode::loop_state::handoff ) {
			// A budget handoff is the more specific cause; a denial is checked
			// after it, matching run_exec's mapping.
			if ( loop_->budget( ).exhausted( ) ) {
				code = cli::exit_code::budget_exhausted;
			} else if ( loop_->permission_denied( ) ) {
				code = cli::exit_code::permission_denied;
			}
		}

		return code;
	}

	auto run_session( const std::vector< std::string >& arguments,
		const std::function< std::optional< std::string >( ) >& input,
		const loop_factory& factory ) -> int {
		auto parsed = parse_exec_options( arguments );

		if ( !parsed ) {
			std::fprintf( stderr, "mcode: %s\n\n", parsed.error( ).msg.c_str( ) );
			std::fputs( usage_text( "mcode" ).c_str( ), stderr );

			return to_int( cli::exit_code::usage_error );
		}

		if ( !parsed->unknown_arguments.empty( ) ) {
			std::fprintf( stderr, "mcode: unknown argument '%s'\n\n",
				parsed->unknown_arguments.front( ).c_str( ) );
			std::fputs( usage_text( "mcode" ).c_str( ), stderr );

			return to_int( cli::exit_code::usage_error );
		}

		auto built = factory( *parsed, nullptr );

		if ( !built ) {
			std::fprintf( stderr, "mcode: %s\n", built.error( ).msg.c_str( ) );

			return to_int( cli::exit_code_for( built.error( ).code ) );
		}

		auto turn = session{ *built };

		auto last_code = cli::exit_code::success;

		while ( true ) {
			const auto line = input( );

			if ( !line ) {
				// Ctrl+D at an empty prompt exits with the last turn's code.
				break;
			}

			if ( line->empty( ) ) {
				continue;
			}

			last_code = turn.run_turn( *line );

			// Ctrl+C mid-turn maps to `interrupted` and returns to the
			// prompt; the session continues.
		}

		return to_int( last_code );
	}

}
