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
#include "mcode/support/json.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"
#include "mcode/agent/loop.hxx"
#include "cli_session.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/cli/repl.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/fs/snapshot.hxx"
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

	// In JSON mode the bus subscription emits the deltas, so they are not mirrored here.
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

auto run_exec( const std::vector< std::string >& arguments ) -> int {
	auto parsed = mcode::cli::parse_exec_options( arguments );

	if ( !parsed ) {
		std::fprintf( stderr, "mcode: %s\n\n", parsed.error( ).msg.c_str( ) );
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	if ( !parsed->unknown_arguments.empty( ) ) {
		std::fprintf( stderr, "mcode: unknown argument '%s'\n\n",
			parsed->unknown_arguments.front( ).c_str( ) );
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	// A listing runs no turn, so it is answered before any config, provider or key work:
	// it must succeed on a machine whose model is not configured yet.
	if ( parsed->list_sessions ) {
		return mcode::cli::to_int( mcode::cli::print_sessions(
			parsed->working_directory.empty( )
				? std::filesystem::current_path( )
				: std::filesystem::path{ parsed->working_directory } ) );
	}

	// A named session is resolved before any provider or key work, so a mistyped id is
	// reported as the argument error it is -- naming the id -- rather than as whatever
	// the provider lookup complains about first. A resume never falls back to a fresh
	// session, so this is a hard stop.
	if ( !parsed->resume_session.empty( ) || parsed->continue_session ) {
		const auto root = parsed->working_directory.empty( )
			? std::filesystem::current_path( )
			: std::filesystem::path{ parsed->working_directory };

		const auto existing = mcode::cli::resolve_session( root, parsed->resume_session );

		if ( !existing ) {
			std::fprintf( stderr, "mcode: %s\n", existing.error( ).msg.c_str( ) );

			return mcode::cli::to_int(
				mcode::cli::exit_code_for( existing.error( ).code ) );
		}
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
		const auto message = mcode::config::unconfigured_message( "provider" );

		std::fprintf( stderr, "mcode: %s", message.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, "no provider configured" );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	if ( model_name.empty( ) ) {
		const auto message = mcode::config::unconfigured_message( "model" );

		std::fprintf( stderr, "mcode: %s", message.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, "no model configured" );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	auto registry = mcode::model::provider_registry{ };
	auto bus = mcode::events::bus{ };
	auto hooks = mcode::ext::hook_registry{ bus };
	auto tool_registry = mcode::tool_registry{ };

	const auto workspace_path = parsed->working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed->working_directory };

	// A worktree is created before anything else resolves the root, so every path the run
	// touches - the workspace, the instruction chain, the tools - points into it.
	auto effective_workspace = workspace_path;

	if ( parsed->worktree ) {
		auto created = mcode::cli::create_worktree( workspace_path, parsed->worktree_name );

		if ( !created ) {
			std::fprintf( stderr, "mcode: %s\n", created.error( ).msg.c_str( ) );
			stream.emit_run_end( mcode::cli::exit_code_for( created.error( ).code ),
				created.error( ).msg );

			return mcode::cli::to_int( mcode::cli::exit_code_for( created.error( ).code ) );
		}

		effective_workspace = *created;

		std::fprintf( stderr, "mcode: worktree %s\n", effective_workspace.string( ).c_str( ) );
	}

	auto skills_options = mcode::skills::session_context_options{ };
	skills_options.workspace = effective_workspace;

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

	if ( parsed->verbose ) {
		for ( const auto& warning : skills_context.warnings ) {
			std::fprintf( stderr, "mcode: %s\n", warning.c_str( ) );
		}
	}

	// Must outlive the loop: the registered tools close over it.
	auto extensions = mcode::ext::load_result{ };

	// Read by the MCP connect step after the loader has run.
	auto extension_servers = mcode::ext::mcp_server_store{ };

	// Opened before the loader: the surface's `fs.*` entries use this same context.
	auto space = mcode::workspace::open( effective_workspace );

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
		// The skills outlive the extensions, which resolve `skill_read` against them.
		options.register_api = mcode::ext::default_register_api( tool_registry,
			{ .skills = &skills_context.skills, .servers = &extension_servers,
				.config = &*config, .space = &*space, .reads = &reads,
				.permissions = &engine } );

		auto roots = mcode::ext::default_roots( effective_workspace );

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

	const auto caps = mcode::model::resolve_capabilities( model_name, &*config );

	if ( !caps ) {
		const auto message = "model '" + model_name
			+ "' has no capabilities; price it in your config under "
			+ mcode::model::capability_config_section( model_name )
			+ ", or use a model the built-in table knows -- refusing to run unpriced";

		std::fprintf( stderr, "mcode: %s\n", message.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, message );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

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

	// Loaded read-only with every allow dropped: a cloned repo cannot grant itself permissions.
	auto project_store = mcode::perm::remember_store{ space->root( ) / ".mcode"
		/ "permissions.json" };

	auto headless_source = mcode::perm::headless_approval_source{ };
	auto terminal_source = mcode::perm::terminal_approval_source{ };

	// `terminal_size` reports stdout's console, so it succeeds exactly when a human can answer.
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
		engine_options.plan_mode = parsed->plan;

		// The permissive default is for a human who can see the disclaimer and use /undo.
		// A headless `--json` run has neither, so it keeps the engine's conservative default
		// and fails closed on anything the rules do not already allow.
		engine_options.approval = parsed->approval.empty( )
			? config->get_string( "sandbox.approval" ).value_or(
				std::string{ interactive ? "never" : "on-request" } )
			: parsed->approval;

		// --yolo forces the permissive mode; --ask is applied after it so the explicit
		// request to be prompted wins over every permissive flag.
		if ( parsed->yolo ) {
			engine_options.approval = "never";
		}

		if ( parsed->ask ) {
			engine_options.approval = "on-request";
			engine_options.yolo = false;
		}

		engine.set_options( engine_options );
		engine.set_approval_source( interactive
			? static_cast< mcode::perm::approval_source* >( &terminal_source )
			: static_cast< mcode::perm::approval_source* >( &headless_source ) );

		for ( const auto& dir : parsed->add_dirs ) {
			engine.add_root( dir );
		}

		// Read from the merged config: reading a raw layer would bypass the never-widen rule.
		auto deny_rules = config->get_string_array( "permissions.deny" );
		auto ask_rules = config->get_string_array( "permissions.ask" );

		if ( !deny_rules || !ask_rules ) {
			const auto problem = !deny_rules ? deny_rules.error( ) : ask_rules.error( );

			std::fprintf( stderr, "mcode: %s\n", problem.msg.c_str( ) );
			stream.emit_run_end( mcode::cli::exit_code::usage_error, problem.msg );

			return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
		}

		engine.add_config_rules( mcode::perm::rule_scope::user, *deny_rules, *ask_rules );

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

	auto run_id = std::to_string(
		static_cast< long long >( mcode::support::epoch_milliseconds( ) ) );

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

	auto provider = *descriptor;

	if ( base_url ) {
		provider.endpoint = *base_url;
	}

	auto log = mcode::event_log{ };

	// The session file is opened before the turn runs, so every event from the first
	// step lands on disk. A resume names its file; an unknown id or a workspace with no
	// session at all is refused here, before any model call is made.
	const auto session = mcode::cli::open_session_for_run( *parsed, space->root( ) );

	if ( !session ) {
		std::fprintf( stderr, "mcode: %s\n", session.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code_for( session.error( ).code ),
			session.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code_for( session.error( ).code ) );
	}

	const auto resumed = !parsed->resume_session.empty( ) || parsed->continue_session;

	if ( const auto opened = log.open( session->path ); !opened ) {
		std::fprintf( stderr, "mcode: %s\n", opened.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code_for( opened.error( ).code ),
			opened.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code_for( opened.error( ).code ) );
	}

	std::fputs( mcode::cli::start_session_log( log, *session, resumed ).c_str( ), stderr );

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
	dependencies.platform_name = std::string{ mcode::cli::PLATFORM_NAME };
	dependencies.permissions = &engine;

	dependencies.instruction_chain = skills_context.chain.text;
	dependencies.skill_index = skills_context.skill_index;

	// A headless run edits files too, so it gets the same undo store the interactive path
	// has; without it a `--json` run would have no rollback at all.
	std::optional< mcode::snapshot_store > snapshots;

	if ( auto data = mcode::platform::app_data_path( mcode::platform::data_kind::data ) ) {
		snapshots.emplace( *data / "snapshots" );
		dependencies.snapshots = &*snapshots;
	}

	// the loop drives the timers: one pump per step, on the thread the VM belongs to.
	dependencies.pump_timers = [ &extensions ]( ) { extensions.pump_timers( ); };

	// Must outlive the loop: the supervisors own the child processes the loop dispatches into.
	auto mcp_servers = mcode::mcp::server_set{ };

	auto loop = mcode::agent_loop{ dependencies };

	// A resumed session continues its conversation. The transcript is rebuilt from
	// the log and seeded before the turn runs, so the model picks up where it left
	// off instead of starting from an empty history.
	if ( resumed ) {
		auto restored = mcode::cli::restore_transcript( log );

		if ( !restored.empty( ) ) {
			const auto carried = restored.size( );

			loop.seed_history( std::move( restored ) );

			std::fprintf( stderr, "mcode: carried %zu message(s) into this turn\n", carried );
		}
	}

	for ( auto& [ name, handler ] : sink.take( ) ) {
		loop.register_handler( name, std::move( handler ) );
	}

	// A registry definition alone is not enough: the loop reports "no handler registered".
	for ( const auto& [ name, owner ] : extensions.tool_owners ) {
		loop.register_handler( name, [ &extensions, tool_name = name ](
			const std::string_view arguments_json ) -> mcode::result< std::string > {
			return extensions.invoke( tool_name, arguments_json );
		} );
	}

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

	const auto code = mcode::cli::exit_code_for_run( *outcome, loop.budget( ).exhausted( ),
		loop.permission_denied( ) );

	if ( parsed->verbose ) {
		const auto usage = client.accumulated_usage( );
		const auto cost = mcode::model::compute_cost( *caps, usage );

		std::fprintf( stderr, "mcode: %d steps, %lld in, %lld out, $%.4f\n",
			static_cast< int >( outcome->steps ),
			static_cast< long long >( usage.input ), static_cast< long long >( usage.output ),
			cost );
	}

	// A `--json` consumer reads one JSON object per line, so the blank line that
	// separates the answer from the summary in the human view is not written
	// there.
	if ( !parsed->json ) {
		std::fputc( '\n', stdout );
	}

	auto summary = std::string{ "run ended in state " };

	if ( outcome->final_state == mcode::loop_state::done ) {
		summary = "completed";
	} else {
		summary += to_string( outcome->final_state );

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
