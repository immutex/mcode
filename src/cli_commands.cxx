// The headless CLI surface. Split from main.cxx, which is the startup smoke test:
// these are real command handlers, and the smoke test is not.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "mcode/tui/approval_tui.hxx"
#include "mcode/tui/editor.hxx"
#include "mcode/tui/frame.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"
#include "mcode/agent/loop.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/cli/repl.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/mcp/connect.hxx"
#include "mcode/model/capabilities.hxx"
#include "mcode/model/http_client.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/net/http_client.hxx"
#include "mcode/perm/approval_headless.hxx"
#include "mcode/perm/approval_terminal.hxx"
#include "mcode/perm/permission.hxx"
#include "mcode/perm/store.hxx"
#include "mcode/platform/seams.hxx"
#include "mcode/skills/session_context.hxx"
#include "mcode/support/config.hxx"
#include "mcode/support/time.hxx"
#include "mcode/tools/context.hxx"
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

	const auto workspace_path = parsed->working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed->working_directory };

	// Sections 10 and 11: the repository's instructions and the skill index.
	// Assembled once, here, because both are session-start only -- recomputing
	// either per turn would invalidate the cached request prefix.
	//
	// The discovered skills must outlive every loaded extension, because the
	// `skills` extension resolves `skill_read` against this object.
	auto skills_options = mcode::skills::session_context_options{ };
	skills_options.workspace = workspace_path;

	const auto data_directory = mcode::platform::app_data_path( mcode::platform::data_kind::config );
	const auto bundled_directory = mcode::platform::executable_directory( );

	if ( data_directory ) {
		skills_options.user_agents_file = *data_directory / "AGENTS.md";
		skills_options.user_skills_root = *data_directory / "skills";
	}

	if ( bundled_directory ) {
		skills_options.org_agents_file = *bundled_directory / "AGENTS.md";
		skills_options.extension_roots = { *bundled_directory / "extensions" };
	}

	auto skills_context = mcode::skills::assemble_session_context( skills_options );

	if ( parsed->verbose ) {
		for ( const auto& warning : skills_context.warnings ) {
			std::fprintf( stderr, "mcode: %s\n", warning.c_str( ) );
		}
	}

	// Declared outside the branch: the loaded extensions own the closures their
	// tools run through, so the result has to outlive the loop.
	auto extensions = mcode::ext::load_result{ };

	// Declared here for the same lifetime reason: the store receives every
	// server an extension declares while loading, and the MCP connect step
	// reads it after the loader has run. It owns nothing; the supervisors live
	// in the server set below.
	auto extension_servers = mcode::ext::mcp_server_store{ };

	// The workspace and its collaborators are opened BEFORE the loader runs:
	// the surface's `fs.*` entries dispatch through the same tool context the
	// model's own file tools use, and that context is assembled from these.
	auto space = mcode::workspace::open( workspace_path );

	if ( !space ) {
		std::fprintf( stderr, "mcode: %s\n", space.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, space.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	auto reads = mcode::tools::session_reads{ };

	const auto user_data = mcode::platform::app_data_path( mcode::platform::data_kind::config );

	auto user_store = mcode::perm::remember_store{ user_data
		? *user_data / "permissions.json"
		: std::filesystem::path{ ".mcode/permissions.json" } };

	auto engine = mcode::perm::permission_engine{ *space, &user_store };

	if ( !parsed->no_extensions ) {
		auto options = mcode::ext::loader_options{ };
		options.register_api = mcode::ext::default_register_api( tool_registry,
			{ .skills = &skills_context.skills, .servers = &extension_servers,
				.config = &*config, .space = &*space, .reads = &reads,
				.permissions = &engine } );

		auto roots = mcode::ext::default_roots( workspace_path );

		// The shipped providers are declared by an extension, not compiled in,
		// and the loader only knows the workspace and user roots. The bundled
		// copy lives beside the binary, so it is added here explicitly.
		//
		// Through the platform seam: the directory of the running binary is
		// Win32's GetModuleFileNameA, macOS's _NSGetExecutablePath and Linux's
		// /proc/self/exe, and `24` requires that decision to live in `platform`
		// rather than in portable code. A missing directory is not fatal -- the
		// bundled extensions are simply absent, which the loader already reports.
		if ( bundled_directory ) {
			roots.push_back( *bundled_directory / "extensions" );
		}

		extensions = mcode::ext::load_extensions( roots, registry, hooks, options );

		if ( parsed->verbose ) {
			for ( const auto& failure : extensions.report.failed ) {
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

	// Two stores, not one. Answers the user gives with `[a]` are remembered in
	// their own file, which follows them between repositories; the repository's
	// own `.mcode/permissions.json` is loaded read-only and has every allow
	// dropped, so a cloned repository cannot grant itself permissions -- and
	// cannot edit the user's answers either.
	auto project_store = mcode::perm::remember_store{ space->root( ) / ".mcode"
		/ "permissions.json" };

	auto headless_source = mcode::perm::headless_approval_source{ };
	auto terminal_source = mcode::perm::terminal_approval_source{ };

	// A prompt only where someone can answer it. `terminal_size` reports the
	// size of stdout's console, so it succeeds exactly when stdout is a
	// terminal -- which is the condition, and asking it through the seam that
	// already owns that question is better than a second platform branch here.
	const auto interactive = !parsed->json && mcode::platform::terminal_size( ).has_value( );

	if ( parsed->verbose ) {
		std::fprintf( stderr, "mcode: sandbox: %s (filesystem %s, network %s)\n",
			mcode::platform::sandbox_mechanism( ).data( ),
			mcode::platform::to_string( mcode::platform::sandbox_capability_level( ) ).data( ),
			mcode::platform::to_string( mcode::platform::sandbox_network_level( ) ).data( ) );
	}

	{
		auto engine_options = mcode::perm::permission_engine::options{ };
		engine_options.yolo = parsed->yolo;
		engine_options.headless = !interactive;
		engine_options.approval = parsed->approval.empty( )
			? config->get_string( "sandbox.approval" ).value_or( std::string{ "on-request" } )
			: parsed->approval;

		if ( parsed->yolo ) {
			engine_options.approval = "never";
		}

		engine.set_options( engine_options );
		engine.set_approval_source( interactive
			? static_cast< mcode::perm::approval_source* >( &terminal_source )
			: static_cast< mcode::perm::approval_source* >( &headless_source ) );

		for ( const auto& dir : parsed->add_dirs ) {
			engine.add_root( dir );
		}

		// The user's own deny and ask rules. Read from the merged config, never
		// from a raw layer: the never-widen rule is enforced during the merge,
		// and reading a layer directly would bypass it.
		engine.add_config_rules( mcode::perm::rule_scope::user,
			config->get_string_array( "permissions.deny" ),
			config->get_string_array( "permissions.ask" ) );

		// A store that cannot be read is not a reason to run with no policy:
		// the engine already reports it, and a failed load leaves the rules
		// that were merged above in place.
		if ( const auto loaded = engine.load_store( ); !loaded ) {
			std::fprintf( stderr, "mcode: %s\n", loaded.error( ).msg.c_str( ) );
		}

		if ( const auto loaded = engine.load_project_store( project_store ); !loaded ) {
			std::fprintf( stderr, "mcode: %s\n", loaded.error( ).msg.c_str( ) );
		}

		for ( const auto& warning : engine.warnings( ) ) {
			std::fprintf( stderr, "mcode: %s\n", warning.c_str( ) );
		}
	}

	auto run_id = std::to_string( static_cast< long long >( mcode::support::epoch_milliseconds( ) ) );

	auto tools_context = mcode::tools::tool_context{ };
	tools_context.space = &*space;
	tools_context.reads = &reads;
	tools_context.permissions = &engine;
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
	dependencies.permissions = &engine;

	// Sections 10 and 11. The index is emitted only when `skill_read` is
	// actually registered, which the prompt builder checks itself -- under
	// `--no-extensions` there is no such tool, so the section is absent without
	// this code having to know that.
	dependencies.instruction_chain = skills_context.chain.text;
	dependencies.skill_index = skills_context.skill_index;

	// Declared before the loop: the supervisors own the child processes and the
	// handlers the loop dispatches into, so they must outlive it. Destruction
	// runs each supervisor's graceful shutdown after the loop is gone.
	auto mcp_servers = mcode::mcp::server_set{ };

	auto loop = mcode::agent_loop{ dependencies };

	for ( auto& [ name, handler ] : sink.take( ) ) {
		loop.register_handler( name, std::move( handler ) );
	}

	// An extension's tool is a definition in the registry plus a closure in the
	// extension's VM. Without this the loop finds the definition and reports
	// "no handler registered", which is what happened the first time an
	// extension registered a *tool* rather than a provider.
	for ( const auto& [ name, owner ] : extensions.tool_owners ) {
		loop.register_handler( name, [ &extensions, tool_name = name ](
			const std::string_view arguments_json ) -> mcode::result< std::string > {
			return extensions.invoke( tool_name, arguments_json );
		} );
	}

	// The MCP servers: parse the merged config, merge the extension-declared
	// set, start the enabled ones, register their tools and handlers.
	const auto connected = mcode::mcp::connect_servers(
		mcode::mcp::connect_input{ .config_values = &config->keys( ),
			.extension_servers = &extension_servers, .registry = &tool_registry,
			.loop = &loop, .owned = &mcp_servers } );

	if ( !connected ) {
		std::fprintf( stderr, "mcode: %s\n", connected.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code_for( connected.error( ).code ),
			connected.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code_for( connected.error( ).code ) );
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
		// A handoff caused by the budget is resumable and exits differently from one
		// that merely had no verification gate. Asked of the budget, not inferred
		// from the log's wording: a reason string that changes would silently turn a
		// budget exit into a success.
		//
		// A denial is checked after the budget because the budget is the more
		// specific cause: a run that ran out of steps *and* was denied is a budget
		// exit. The flag is cleared by any later successful call, so a denial the
		// run recovered from does not reach here.
		if ( loop.budget( ).exhausted( ) ) {
			code = mcode::cli::exit_code::budget_exhausted;
		} else if ( loop.permission_denied( ) ) {
			code = mcode::cli::exit_code::permission_denied;
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

		// The loop records why it stopped. Without it a provider failure reports a
		// bare state name, which is the silent failure the exit codes exist to
		// prevent: exit 4 tells the caller nothing about the cause.
		if ( !outcome->summary_json.empty( ) ) {
			summary += ": ";
			summary += outcome->summary_json;
		}
	}

	if ( code != mcode::cli::exit_code::success ) {
		std::fprintf( stderr, "mcode: %s\n", summary.c_str( ) );
	}

	stream.emit_run_end( code, summary );

	return mcode::cli::to_int( code );
}

// The loop factory the REPL drives. The same construction order run_exec
// uses; the parts outlive the loop by declaration order inside the factory's
// static holder, which keeps one session per process.
auto build_interactive_loop( const mcode::cli::exec_options& parsed,
	mcode::perm::approval_source* interactive_approval )
	-> mcode::result< mcode::agent_loop > {
	struct session_parts {
		mcode::model::provider_registry providers;
		mcode::events::bus bus;
		mcode::ext::hook_registry hooks;
		mcode::tool_registry tools;
		std::optional< mcode::workspace > space;
		mcode::tools::session_reads reads;
		std::optional< mcode::perm::remember_store > user_store;
		std::optional< mcode::perm::remember_store > project_store;
		std::optional< mcode::perm::permission_engine > engine;
		mcode::ext::load_result extensions;
		mcode::ext::mcp_server_store extension_servers;
		mcode::mcp::server_set mcp_servers;
		mcode::net::http_client transport;
		mcode::model::http_model_client client;
		mcode::event_log log;
		mcode::session_budget budget;
		mcode::agent_loop::dependencies loop_deps;

		// The plain-mode prompt source. It reads `std::cin`, which is correct
		// here because plain mode never puts the console into raw mode; the TUI
		// path passes its own raw-mode source instead.
		mcode::perm::terminal_approval_source terminal_source;

		session_parts( ) : hooks( bus ), client( transport ) { }
	};

	static auto parts = std::optional< session_parts >{ };

	// One session per process. A second factory call would build an unwired
	// loop over shared parts -- every tool call would report "no handler
	// registered" -- so it is refused rather than half-built.
	if ( parts ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"an interactive session is already running in this process" ) );
	}

	parts.emplace( );

	const auto workspace_path = parsed.working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed.working_directory };

	const auto config = mcode::config::load( workspace_path );

	if ( !config ) {
		return std::unexpected( config.error( ) );
	}

	const auto provider_name = parsed.model.empty( )
		? config->get_string( "model.provider" ).value_or( std::string{ } )
		: parsed.model;
	const auto model_name = config->get_string( "model.model" ).value_or( std::string{ } );
	const auto api_key_env = config->get_string( "model.api_key_env" );
	const auto base_url = config->get_string( "model.base_url" );

	if ( provider_name.empty( ) ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"no provider configured; set [model] provider in config.toml" ) );
	}

	if ( model_name.empty( ) ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"no model configured; set [model] model in config.toml" ) );
	}

	auto skills_options = mcode::skills::session_context_options{ };
	skills_options.workspace = workspace_path;

	const auto data_directory = mcode::platform::app_data_path(
		mcode::platform::data_kind::config );
	const auto bundled_directory = mcode::platform::executable_directory( );

	if ( data_directory ) {
		skills_options.user_agents_file = *data_directory / "AGENTS.md";
		skills_options.user_skills_root = *data_directory / "skills";
	}

	if ( bundled_directory ) {
		skills_options.org_agents_file = *bundled_directory / "AGENTS.md";
		skills_options.extension_roots = { *bundled_directory / "extensions" };
	}

	auto skills_context = mcode::skills::assemble_session_context( skills_options );

	// The workspace and its collaborators are built BEFORE the loader runs, so
	// the extension surface's `fs.*` entries dispatch through the same tool
	// context the model's own file tools use.
	auto space = mcode::workspace::open( workspace_path );

	if ( !space ) {
		return std::unexpected( space.error( ) );
	}

	parts->space = std::move( *space );

	const auto user_data = mcode::platform::app_data_path(
		mcode::platform::data_kind::config );

	parts->user_store = mcode::perm::remember_store{ user_data
		? *user_data / "permissions.json"
		: std::filesystem::path{ ".mcode/permissions.json" } };

	parts->engine = mcode::perm::permission_engine{ *parts->space, &*parts->user_store };

	if ( !parsed.no_extensions ) {
		auto options = mcode::ext::loader_options{ };
		options.register_api = mcode::ext::default_register_api( parts->tools,
			{ .skills = &skills_context.skills, .servers = &parts->extension_servers,
				.config = &*config, .space = &*parts->space, .reads = &parts->reads,
				.permissions = &*parts->engine } );

		auto roots = mcode::ext::default_roots( workspace_path );

		if ( bundled_directory ) {
			roots.push_back( *bundled_directory / "extensions" );
		}

		parts->extensions = mcode::ext::load_extensions( roots, parts->providers,
			parts->hooks, options );
	}

	const auto* descriptor = parts->providers.find( provider_name );

	if ( descriptor == nullptr ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"no provider named '" + provider_name
			+ "' is registered; check [model] provider and the loaded extensions" ) );
	}

	const auto caps = mcode::model::lookup_capabilities( model_name );

	if ( !caps ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"model '" + model_name
			+ "' is not in the compiled-in capabilities table; refusing to run unpriced" ) );
	}

	auto auth = descriptor->auth;

	if ( auth.from == mcode::model::auth_spec::source::environment && api_key_env ) {
		auth.name = *api_key_env;
	}

	const auto api_key = mcode::model::resolve_api_key( auth,
		[ &config ]( const std::string_view key ) { return config->get_string( key ); } );

	if ( !api_key ) {
		return std::unexpected( api_key.error( ) );
	}

	parts->project_store = mcode::perm::remember_store{
		parts->space->root( ) / ".mcode" / "permissions.json" };

	const auto interactive = !parsed.json &&
		mcode::platform::terminal_size( ).has_value( );

	{
		auto engine_options = mcode::perm::permission_engine::options{ };
		engine_options.yolo = parsed.yolo;
		engine_options.headless = !interactive;
		engine_options.approval = parsed.approval.empty( )
			? config->get_string( "sandbox.approval" ).value_or( std::string{ "on-request" } )
			: parsed.approval;

		if ( parsed.yolo ) {
			engine_options.approval = "never";
		}

		parts->engine->set_options( engine_options );

		// The approval source the session actually asks through. Without one
		// the engine's null-source path denies every ask, which would make
		// the interactive session refuse every tool call. Plain mode with a
		// terminal is still interactive, so it gets the stdin prompt rather
		// than a deny: only a genuinely headless run has no one to ask.
		if ( interactive_approval != nullptr ) {
			parts->engine->set_approval_source( interactive_approval );
		} else if ( interactive ) {
			parts->engine->set_approval_source( &parts->terminal_source );
		}

		for ( const auto& dir : parsed.add_dirs ) {
			parts->engine->add_root( dir );
		}

		parts->engine->add_config_rules( mcode::perm::rule_scope::user,
			config->get_string_array( "permissions.deny" ),
			config->get_string_array( "permissions.ask" ) );

		if ( const auto loaded = parts->engine->load_store( ); !loaded ) {
			std::fprintf( stderr, "mcode: %s\n", loaded.error( ).msg.c_str( ) );
		}

		if ( const auto loaded = parts->engine->load_project_store(
			*parts->project_store ); !loaded ) {
			std::fprintf( stderr, "mcode: %s\n", loaded.error( ).msg.c_str( ) );
		}
	}

	const auto run_id = std::to_string(
		static_cast< long long >( mcode::support::epoch_milliseconds( ) ) );

	auto tools_context = mcode::tools::tool_context{ };
	tools_context.space = &*parts->space;
	tools_context.reads = &parts->reads;
	tools_context.permissions = &*parts->engine;
	tools_context.run_id = run_id;
	tools_context.headless = parsed.json;

	auto sink = mcode::tools::vector_sink{ };

	const auto registered = mcode::tools::register_core_tools( parts->tools, sink,
		tools_context );

	if ( !registered ) {
		return std::unexpected( registered.error( ) );
	}

	if ( parsed.max_steps > 0 ) {
		parts->budget.max_steps = parsed.max_steps;
	}

	if ( parsed.max_budget_usd > 0.0 ) {
		parts->budget.max_usd = parsed.max_budget_usd;
	}

	auto provider = *descriptor;

	if ( base_url ) {
		provider.endpoint = *base_url;
	}

	parts->loop_deps.registry = &parts->tools;
	parts->loop_deps.client = &parts->client;
	parts->loop_deps.log = &parts->log;
	parts->loop_deps.bus = &parts->bus;
	parts->loop_deps.budget = parts->budget;
	parts->loop_deps.model_name = model_name;
	parts->loop_deps.caps = *caps;
	parts->loop_deps.provider_name = provider_name;
	parts->loop_deps.provider = provider;
	parts->loop_deps.api_key = *api_key;
	parts->loop_deps.workspace_root = parts->space->root( ).string( );
	parts->loop_deps.platform_name = std::string{ PLATFORM_NAME };
	parts->loop_deps.permissions = &*parts->engine;
	parts->loop_deps.instruction_chain = skills_context.chain.text;
	parts->loop_deps.skill_index = skills_context.skill_index;

	auto loop = mcode::agent_loop{ parts->loop_deps };

	for ( auto& [ name, handler ] : sink.take( ) ) {
		loop.register_handler( name, std::move( handler ) );
	}

	for ( const auto& [ name, owner ] : parts->extensions.tool_owners ) {
		loop.register_handler( name,
			[ &extensions = parts->extensions, tool_name = name ](
				const std::string_view arguments_json ) -> mcode::result< std::string > {
			return extensions.invoke( tool_name, arguments_json );
		} );
	}

	const auto connected = mcode::mcp::connect_servers(
		mcode::mcp::connect_input{ .config_values = &config->keys( ),
			.extension_servers = &parts->extension_servers, .registry = &parts->tools,
			.loop = &loop, .owned = &parts->mcp_servers } );

	if ( !connected ) {
		return std::unexpected( connected.error( ) );
	}

	return loop;
}

// The interactive session. The construction sequence is run_exec's, minus the
// single-run JSON stream and the single run( ) call: the loop stays alive so
// consecutive turns share history, the remember store and the session rules.
// Returns the process exit code.
// The interactive surface. With a terminal: a tty_session in raw mode, the
// multi-line editor producing input lines, the render coordinator consuming
// the event queue the loop's bus handlers feed, and the turn running on a
// worker thread so a slow tool call cannot freeze the repaint. The loop
// thread stays the only bus publisher -- its handlers push copies into the
// queue, and only the render side drains it.
//
// Without a terminal, or under MCODE_TUI=plain, the session falls back to
// the plain line reader: an explicit branch, not a silent degradation.
auto run_repl( const std::vector< std::string >& arguments ) -> int {
	const auto* requested = std::getenv( "MCODE_TUI" );
	const auto plain_requested = requested != nullptr &&
		std::string_view{ requested } == "plain";

	const auto plain_reader = []( ) -> std::optional< std::string > {
		auto line = std::string{ };

		if ( !std::getline( std::cin, line ) ) {
			return std::nullopt;
		}

		while ( !line.empty( ) && line.back( ) == '\r' ) {
			line.pop_back( );
		}

		return line;
	};

	if ( plain_requested || !mcode::platform::terminal_size( ).has_value( ) ) {
		return mcode::cli::run_session( arguments, plain_reader, build_interactive_loop );
	}

	auto session_tty = mcode::tui::tty_session::create( );

	if ( !session_tty ) {
		// Raw mode unavailable: the plain reader still gives a working
		// session rather than a broken one.
		return mcode::cli::run_session( arguments, plain_reader, build_interactive_loop );
	}

	auto parsed = mcode::cli::parse_exec_options( arguments );

	if ( !parsed || !parsed->unknown_arguments.empty( ) ) {
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	// The approval prompt reads through the same raw-mode line reader the
	// editor uses, so it works under raw mode where std::getline would
	// block forever on \r.
	auto approval = mcode::tui::ui_approval_source{
		[ &session_tty ]( ) { return session_tty->read_line( 600'000 ); } };

	auto built = build_interactive_loop( *parsed, &approval );

	if ( !built ) {
		std::fprintf( stderr, "mcode: %s\n", built.error( ).msg.c_str( ) );

		return mcode::cli::to_int( mcode::cli::exit_code_for( built.error( ).code ) );
	}

	auto& loop = *built;
	auto turn = mcode::cli::session{ loop };

	auto queue = mcode::tui::event_queue{ };
	auto coordinator = mcode::tui::render_coordinator{ };
	coordinator.set_capabilities( session_tty->caps( ) );

	const auto measured = session_tty->size( );
	coordinator.resize( mcode::tui::LIVE_REGION_ROWS,
		static_cast< std::size_t >( measured.first ) );

	// The bus handlers run on the loop thread (the worker below) and push
	// copies into the queue; the render side is the only reader.
	const auto feed = [&queue]( mcode::tui::event_queue::kind target ) {
		return [&queue, target]( const mcode::events::event& value ) {
			auto item = mcode::tui::event_queue::item{ };
			item.type = target;
			item.text = value.payload_json;

			queue.push( std::move( item ) );
		};
	};

	auto subscriptions = std::vector< mcode::events::bus::subscription_id >{ };
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::assistant_delta, feed( mcode::tui::event_queue::kind::assistant_delta ) ) );
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::tool_call, feed( mcode::tui::event_queue::kind::tool_start ) ) );
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::tool_result, feed( mcode::tui::event_queue::kind::tool_end ) ) );
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::turn_start, feed( mcode::tui::event_queue::kind::turn_start ) ) );
	subscriptions.push_back( loop.bus( ).subscribe(
		mcode::events::kind::turn_end, feed( mcode::tui::event_queue::kind::turn_end ) ) );

	auto last_code = mcode::cli::exit_code::success;
	auto turn_done = std::atomic< bool >{ false };
	// `std::thread`, not `std::jthread`: jthread needs `stop_token`, which
	// libc++ does not provide at every macOS deployment target, and this code
	// joins explicitly anyway -- so the auto-join was never the reason.
	auto worker = std::thread{ };

	auto repaint = [&coordinator, &session_tty]( ) {
		const auto bytes = coordinator.flush( );

		if ( !bytes.empty( ) ) {
			session_tty->write( session_tty->caps( ).synchronized_output
				? mcode::tui::ansi_emitter{ session_tty->caps( ) }.synchronized( bytes )
				: bytes );
		}
	};

	auto pump_until_done = [&]() {
		while ( !turn_done.load( ) ) {
			for ( const auto& item : queue.drain( ) ) {
				coordinator.apply( item );
			}

			repaint( );
			std::this_thread::sleep_for( std::chrono::milliseconds( 80 ) );
		}

		for ( const auto& item : queue.drain( ) ) {
			coordinator.apply( item );
		}

		repaint( );
	};

	auto editor = mcode::tui::input_editor{ };

	while ( true ) {
		// The editor owns the prompt: keys go through it, so multi-line
		// editing, the history ring and the ghost-text suggestion are live.
		// The prompt row renders the pending input between keys.
		auto submitted = std::optional< std::string >{ };

		while ( !submitted ) {
			const auto key = session_tty->read_key( 600'000 );

			if ( key.type == mcode::tui::key_event::kind::exit ) {
				// Ctrl+D ends the session; the editor's exit flag tracks it.
				break;
			}

			if ( key.type == mcode::tui::key_event::kind::interrupt ) {
				// Ctrl+C clears the pending input and returns to the prompt;
				// the session continues.
				auto clear = mcode::tui::input_editor::key_event{ };
				clear.type = mcode::tui::input_editor::key::interrupt;
				std::ignore = editor.handle( clear );

				continue;
			}

			if ( key.type == mcode::tui::key_event::kind::enter ) {
				auto enter = mcode::tui::input_editor::key_event{ };
				enter.type = mcode::tui::input_editor::key::enter;
				submitted = editor.handle( enter );

				continue;
			}

			auto forwarded = mcode::tui::input_editor::key_event{ };

			switch ( key.type ) {
				case mcode::tui::key_event::kind::character:
					forwarded.type = mcode::tui::input_editor::key::character;
					forwarded.text = key.text;

					break;
				case mcode::tui::key_event::kind::backspace:
					forwarded.type = mcode::tui::input_editor::key::backspace;

					break;
				case mcode::tui::key_event::kind::delete_key:
					forwarded.type = mcode::tui::input_editor::key::delete_key;

					break;
				case mcode::tui::key_event::kind::left:
					forwarded.type = mcode::tui::input_editor::key::left;

					break;
				case mcode::tui::key_event::kind::right:
					forwarded.type = mcode::tui::input_editor::key::right;

					break;
				case mcode::tui::key_event::kind::up:
					forwarded.type = mcode::tui::input_editor::key::up;

					break;
				case mcode::tui::key_event::kind::down:
					forwarded.type = mcode::tui::input_editor::key::down;

					break;
				case mcode::tui::key_event::kind::home:
					forwarded.type = mcode::tui::input_editor::key::home;

					break;
				case mcode::tui::key_event::kind::end:
					forwarded.type = mcode::tui::input_editor::key::end;

					break;
				default:
					continue;
			}

			std::ignore = editor.handle( forwarded );
		}

		if ( !submitted || submitted->empty( ) ) {
			if ( !submitted ) {
				break;
			}

			continue;
		}

		turn_done.store( false );

		worker = std::thread{ [ & ]( ) {
			last_code = turn.run_turn( *submitted );
			turn_done.store( true );
		} };

		pump_until_done( );
		worker.join( );

		if ( last_code == mcode::cli::exit_code::interrupted ) {
			continue;
		}
	}

	return mcode::cli::to_int( last_code );
}
