// The headless CLI surface. Split from main.cxx, which is the startup smoke test:
// these are real command handlers, and the smoke test is not.
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>
#endif

#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/model/capabilities.hxx"
#include "mcode/model/http_client.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/support/config.hxx"
#include "mcode/support/time.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/tools/exec_policy.hxx"
#include "mcode/tools/register.hxx"

namespace {

#if defined( _WIN32 )
	inline constexpr std::string_view PLATFORM_NAME = "windows";
#elif defined( __APPLE__ )
	inline constexpr std::string_view PLATFORM_NAME = "macos";
#else
	inline constexpr std::string_view PLATFORM_NAME = "linux";
#endif

	// Forwards the inner client's events to the loop. In human mode it mirrors
	// text deltas to stdout as they arrive; in JSON mode the bus subscription
	// below emits them as lines instead, so raw text never breaks the stream.
	class streaming_client final : public mcode::model::model_client {
	public:
		streaming_client( mcode::model::model_client& inner, std::FILE* out, const bool json )
			: inner_( inner ), out_( out ), json_( json ) { }

		auto stream( const mcode::model::stream_request& request,
			const mcode::model::event_sink& sink ) -> mcode::status override {
			return inner_.stream( request,
				[ this, &sink ]( const mcode::model::chat_event& event ) {
					if ( !json_ && event.type == mcode::model::chat_event::kind::text_delta ) {
						std::fwrite( event.text.data( ), 1, event.text.size( ), out_ );
						std::fflush( out_ );
					}

					sink( event );
				} );
		}

	private:
		mcode::model::model_client& inner_;
		std::FILE* out_;
		bool json_;
	};

}

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

	const auto config = mcode::config::load( parsed->working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed->working_directory } );

	if ( !config ) {
		std::fprintf( stderr, "mcode: %s\n", config.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, config.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	const auto provider_name = parsed->model.empty( )
		? config->get_string( "model.provider" ).value_or( std::string{ } )
		: parsed->model;
	const auto model_name = config->get_string( "model.model" ).value_or( std::string{ } );
	const auto api_key_env = config->get_string( "model.api_key_env" );
	const auto base_url = config->get_string( "model.base_url" );

	if ( provider_name.empty( ) ) {
		std::fprintf( stderr, "mcode: no provider configured; set [model] provider in config.toml\n" );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, "no provider configured" );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	if ( model_name.empty( ) ) {
		std::fprintf( stderr, "mcode: no model configured; set [model] model in config.toml\n" );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, "no model configured" );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	// The provider descriptors are declared by the providers extension, so the
	// loader must have run before this lookup. A name that matches no
	// descriptor is a fatal, named error, never a silent fallback to a default.
	auto registry = mcode::model::provider_registry{ };
	auto bus = mcode::events::bus{ };
	auto hooks = mcode::ext::hook_registry{ bus };
	auto tool_registry = mcode::tool_registry{ };

	if ( !parsed->no_extensions ) {
		auto options = mcode::ext::loader_options{ };
		options.register_api = mcode::ext::default_register_api( tool_registry );

		const auto workspace = parsed->working_directory.empty( )
			? std::filesystem::current_path( )
			: std::filesystem::path{ parsed->working_directory };

		auto roots = mcode::ext::default_roots( workspace );

		// The shipped providers are declared by an extension, not compiled in,
		// and the loader only knows the workspace and user roots. The bundled
		// copy lives beside the binary, so it is added here explicitly.
		char binary_path[ 1024 ] = { };
		const auto binary_size = GetModuleFileNameA( nullptr, binary_path,
			static_cast< DWORD >( sizeof( binary_path ) ) );

		if ( binary_size > 0 && binary_size < sizeof( binary_path ) ) {
			roots.push_back( std::filesystem::path{ binary_path }.parent_path( ) / "extensions" );
		}

		auto loaded = mcode::ext::load_extensions( roots, registry, hooks, options );

		if ( parsed->verbose ) {
			for ( const auto& failure : loaded.report.failed ) {
				std::fprintf( stderr, "mcode: extension '%s' failed to load: %s\n",
					failure.name.c_str( ), failure.reason.c_str( ) );
			}
		}
	}

	const auto* descriptor = registry.find( provider_name );

	if ( descriptor == nullptr ) {
		const auto message = "no provider named '" + provider_name
			+ "' is registered; check [model] provider and the loaded extensions";

		std::fprintf( stderr, "mcode: %s\n", message.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, message );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	// Capabilities are looked up per model id and fail closed: an unknown model
	// would price every turn at zero and silently disable budget enforcement.
	const auto caps = mcode::model::lookup_capabilities( model_name );

	if ( !caps ) {
		const auto message = "model '" + model_name
			+ "' is not in the compiled-in capabilities table; refusing to run unpriced";

		std::fprintf( stderr, "mcode: %s\n", message.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, message );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	// The credential. The descriptor names the source; the environment variable
	// from the config overrides the descriptor's default name.
	auto auth = descriptor->auth;

	if ( auth.from == mcode::model::auth_spec::source::environment && api_key_env ) {
		auth.name = *api_key_env;
	}

	const auto api_key = mcode::model::resolve_api_key( auth,
		[ &config ]( const std::string_view key ) { return config->get_string( key ); } );

	if ( !api_key ) {
		std::fprintf( stderr, "mcode: %s\n", api_key.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, api_key.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	auto transport = mcode::net::http_client{ };
	auto client = mcode::model::http_model_client{ transport };
	auto loop_client = streaming_client{ client, stdout, parsed->json };

	// The loop publishes on this bus; in JSON mode the stream carries the
	// progress kinds out as one JSON object per line, flushed as they happen.
	bus.subscribe( mcode::events::kind::assistant_delta, [&]( const mcode::events::event& value ) {
		stream.emit_event( value );
	} );

	bus.subscribe( mcode::events::kind::turn_start, [&]( const mcode::events::event& value ) {
		stream.emit_event( value );
	} );

	bus.subscribe( mcode::events::kind::turn_end, [&]( const mcode::events::event& value ) {
		stream.emit_event( value );
	} );

	bus.subscribe( mcode::events::kind::step_start, [&]( const mcode::events::event& value ) {
		stream.emit_event( value );
	} );

	bus.subscribe( mcode::events::kind::step_end, [&]( const mcode::events::event& value ) {
		stream.emit_event( value );
	} );

	bus.subscribe( mcode::events::kind::tool_call, [&]( const mcode::events::event& value ) {
		stream.emit_event( value );
	} );

	bus.subscribe( mcode::events::kind::tool_result, [&]( const mcode::events::event& value ) {
		stream.emit_event( value );
	} );

	auto space = mcode::workspace::open( parsed->working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed->working_directory } );

	if ( !space ) {
		std::fprintf( stderr, "mcode: %s\n", space.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, space.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	auto reads = mcode::tools::session_reads{ };
	auto policy = mcode::tools::exec_policy{ };
	policy.yolo = parsed->yolo;

	auto run_id = std::to_string( static_cast< long long >( mcode::support::epoch_milliseconds( ) ) );

	auto tools_context = mcode::tools::tool_context{ };
	tools_context.space = &*space;
	tools_context.reads = &reads;
	tools_context.policy = &policy;
	tools_context.run_id = run_id;
	tools_context.headless = parsed->json;

	auto sink = mcode::tools::vector_sink{ };

	const auto registered = mcode::tools::register_core_tools( tool_registry, sink, tools_context );

	if ( !registered ) {
		std::fprintf( stderr, "mcode: %s\n", registered.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code_for( registered.error( ).code ),
			registered.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code_for( registered.error( ).code ) );
	}

	auto budget = mcode::session_budget{ };

	if ( parsed->max_steps > 0 ) {
		budget.max_steps = parsed->max_steps;
	}

	if ( parsed->max_budget_usd > 0.0 ) {
		budget.max_usd = parsed->max_budget_usd;
	}

	// The config endpoint overrides the descriptor's default; this must happen
	// before the descriptor is copied into the loop's dependencies.
	auto provider = *descriptor;

	if ( base_url ) {
		provider.endpoint = *base_url;
	}

	auto log = mcode::event_log{ };

	auto dependencies = mcode::agent_loop::dependencies{ };
	dependencies.registry = &tool_registry;
	dependencies.client = &loop_client;
	dependencies.log = &log;
	dependencies.bus = &bus;
	dependencies.budget = budget;
	dependencies.model_name = model_name;
	dependencies.caps = *caps;
	dependencies.provider_name = provider_name;
	dependencies.provider = provider;
	dependencies.api_key = *api_key;
	dependencies.workspace_root = space->root( ).string( );
	dependencies.platform_name = std::string{ PLATFORM_NAME };

	auto loop = mcode::agent_loop{ dependencies };

	for ( auto& [ name, handler ] : sink.take( ) ) {
		loop.register_handler( name, std::move( handler ) );
	}

	const auto outcome = loop.run( parsed->prompt );

	if ( !outcome ) {
		std::fprintf( stderr, "\nmcode: %s\n", outcome.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code_for( outcome.error( ).code ),
			outcome.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code_for( outcome.error( ).code ) );
	}

	auto code = mcode::cli::exit_code::success;

	if ( outcome->final_state == mcode::loop_state::failed ) {
		code = mcode::cli::exit_code::provider_error;
	} else if ( outcome->final_state == mcode::loop_state::handoff ) {
		// The run's reason lives in the log's final run.end payload; a handoff that
		// the budget caused exits differently from one that merely had no gate.
		auto budget_caused = false;

		for ( const auto& logged : log.events( ) ) {
			if ( logged.kind == "run.end" ) {
				budget_caused = logged.payload_json.find( "budget exhausted" )
					!= std::string::npos;
			}
		}

		if ( budget_caused ) {
			code = mcode::cli::exit_code::budget_exhausted;
		}
	}

	if ( parsed->verbose ) {
		const auto usage = client.accumulated_usage( );
		const auto cost = mcode::model::compute_cost( *caps, usage );

		std::fprintf( stderr, "mcode: %d steps, %lld in, %lld out, $%.4f\n",
			static_cast< int >( outcome->model_calls ),
			static_cast< long long >( usage.input ), static_cast< long long >( usage.output ),
			cost );
	}

	std::fputc( '\n', stdout );

	auto summary = std::string{ "run ended in state " };

	if ( outcome->final_state == mcode::loop_state::done ) {
		summary = "completed";
	} else {
		summary += to_string( outcome->final_state );
	}

	stream.emit_run_end( code, summary );

	return mcode::cli::to_int( code );
}
