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

#include "mcode/cli/exec.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/model/capabilities.hxx"
#include "mcode/model/http_client.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/support/config.hxx"
#include "mcode/support/json.hxx"

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

	auto request = mcode::model::chat_request{ };
	request.model = model_name;
	request.reasoning_effort = mcode::model::effort::medium;

	auto user_message = mcode::model::message{ };
	user_message.speaker = mcode::model::role::user;

	auto text_block = mcode::model::block{ };
	text_block.kind = mcode::model::block_kind::text;
	text_block.text = parsed->prompt;

	user_message.blocks.push_back( std::move( text_block ) );
	request.messages.push_back( std::move( user_message ) );

	auto stream_request = mcode::model::stream_request{ };
	stream_request.request = std::move( request );
	stream_request.provider = *descriptor;
	stream_request.api_key = *api_key;
	stream_request.caps = *caps;

	if ( base_url ) {
		stream_request.provider.endpoint = *base_url;
	}

	auto transport = mcode::net::http_client{ };
	auto client = mcode::model::http_model_client{ transport };

	const auto sent = client.stream( stream_request,
		[ & ]( const mcode::model::chat_event& event ) {
			switch ( event.type ) {
				case mcode::model::chat_event::kind::text_delta:
					std::fwrite( event.text.data( ), 1, event.text.size( ), stdout );
					std::fflush( stdout );
					break;

				case mcode::model::chat_event::kind::thinking_delta:
				case mcode::model::chat_event::kind::tool_call_delta:
				case mcode::model::chat_event::kind::usage:
				case mcode::model::chat_event::kind::turn_done:
				case mcode::model::chat_event::kind::error:
					break;
			}
		} );

	if ( !sent ) {
		std::fprintf( stderr, "\nmcode: %s\n", sent.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code_for( sent.error( ).code ), sent.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code_for( sent.error( ).code ) );
	}

	std::fputc( '\n', stdout );

	const auto cost = mcode::model::compute_cost( *caps, client.accumulated_usage( ) );

	if ( parsed->verbose ) {
		std::fprintf( stderr, "mcode: %lld in, %lld out, $%.4f\n",
			static_cast< long long >( client.accumulated_usage( ).input ),
			static_cast< long long >( client.accumulated_usage( ).output ), cost );
	}

	stream.emit_run_end( mcode::cli::exit_code::success, "completed" );

	return mcode::cli::to_int( mcode::cli::exit_code::success );
}
