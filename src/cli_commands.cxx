// The headless CLI surface. Split from main.cxx, which is the startup smoke test:
// these are real command handlers, and the smoke test is not.
#include <cstdio>
#include <string>
#include <vector>

#include "mcode/cli/exec.hxx"
#include "mcode/events/bus.hxx"

// `mcode exec [options] [prompt]` -- the headless surface.
	// Returns the process exit code.
auto run_exec( const std::vector< std::string >& arguments ) -> int {
	auto parsed = mcode::cli::parse_exec_options( arguments );

	if ( !parsed ) {
		std::fprintf( stderr, "mcode: %s\n\n", parsed.error( ).msg.c_str( ) );
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	// Unknown flags are refused, never ignored: a typo like `--max-step` would
	// otherwise run with the default budget and the user would never know.
	if ( !parsed->unknown_arguments.empty( ) ) {
		std::fprintf( stderr, "mcode: unknown argument '%s'\n\n",
			parsed->unknown_arguments.front( ).c_str( ) );
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	auto stream = mcode::cli::json_stream{ parsed->json };
	stream.emit_run_start( parsed->prompt );

	auto bus = mcode::events::bus{ };
	auto sequence = std::uint64_t{ 0 };

	// Mirror every event onto the JSON stream. This is the coupling the event
	// bus exists for: the TUI, the session log, and the headless stream are all
	// subscribers, and none of them is special.
	bus.subscribe( mcode::events::kind::session_start,
		[&stream, &sequence]( const mcode::events::event& value ) {
			auto mirrored = value;
			mirrored.sequence = sequence++;
			stream.emit_event( mirrored );
		} );

	auto start = mcode::events::event{ };
	start.type = mcode::events::kind::session_start;
	start.payload_json = R"({"headless":true})";
	bus.publish( start );

	auto end = mcode::events::event{ };
	end.type = mcode::events::kind::session_end;
	end.payload_json = R"({"reason":"m0-skeleton"})";
	bus.publish( end );

	// M0 has no model client, so a run cannot yet produce a result. Saying so
	// with the provider-error code is honest; exiting 0 would claim a
	// verification that never ran.
	stream.emit_run_end( mcode::cli::exit_code::provider_error,
		"no model client in M0 (docs/16 M0)" );

	if ( parsed->verbose ) {
		std::fprintf( stderr, "mcode: exec finished (M0 skeleton, no model client)\n" );
	}

return mcode::cli::to_int( mcode::cli::exit_code::provider_error );
}
