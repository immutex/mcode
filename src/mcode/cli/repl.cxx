#include "mcode/cli/repl.hxx"

#include <cstdio>
#include <iostream>
#include <string>
#include <utility>

#include "mcode/support/json.hxx"

namespace mcode::cli {

	auto session::run_turn( const std::string_view task ) -> cli::exit_code {
		const auto outcome = loop_->run( task );

		if ( !outcome ) {
			return cli::exit_code_for( outcome.error( ).code );
		}

		return exit_code_for_run( *outcome, loop_->budget( ).exhausted( ),
			loop_->permission_denied( ) );
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

		// The plain path draws no prompt, so without this the session prints
		// nothing until the first answer arrives and reads as a hang. The
		// model is named because it is the one thing the user cannot see from
		// the prompt: the plain path is the fallback for a terminal the TUI
		// could not take over.
		std::fprintf( stderr, "mcode: interactive session; one prompt per line, Ctrl+D to exit\n" );
		std::fflush( stderr );

		// The plain path has no renderer, so the answer is echoed here. Without
		// this the fallback read input and discarded every response, which is a
		// session that looks like it works and says nothing.
		// The bus owns both subscriptions for as long as the loop lives, so the
		// handles are kept only to make the ownership explicit.
		[[maybe_unused]] auto echo = built->bus( ).subscribe( events::kind::assistant_delta,
			[]( const events::event& value ) {
				// Named `payload` rather than `parsed`: the outer scope holds the
				// parsed options, and shadowing it is a gate failure.
				auto payload = json::document::parse( value.payload_json );

				if ( !payload ) {
					return;
				}

				if ( const auto text = payload->pointer_string( "/text" ); text ) {
					std::fputs( text->c_str( ), stdout );
					std::fflush( stdout );
				}
			} );

		[[maybe_unused]] auto newline = built->bus( ).subscribe( events::kind::turn_end,
			[]( const events::event& ) {
				std::fputc( '\n', stdout );
				std::fflush( stdout );
			} );

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
