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
#include "mcode/agent/subagent.hxx"
#include "mcode/cli/egress_setup.hxx"
#include "mcode/cli/active_loop.hxx"
#include "mcode/cli/tier_config.hxx"
#include "mcode/cli/extension_wiring.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/cli/session.hxx"
#include "mcode/cli/session_state.hxx"
#include "mcode/cli/repl.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/api_cmd.hxx"
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
#include "exec_internal.hxx"

#include "mcode/support/config.hxx"
#include "mcode/support/time.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/tools/register.hxx"

namespace {

	// The workspace root the invocation names, or the current directory.
	[[nodiscard]] auto invocation_root( const mcode::cli::exec_options& parsed )
		-> std::filesystem::path {
		return parsed.working_directory.empty( )
			? std::filesystem::current_path( )
			: std::filesystem::path{ parsed.working_directory };
	}

}

// Everything that must be settled before a single byte of provider work: a bad
// invocation, a listing, or a resume that names no session. Returns the exit code
// when the run should stop there, and nothing when it should proceed.
[[nodiscard]] auto refuse_bad_invocation( const std::vector< std::string >& arguments,
	mcode::cli::exec_options& parsed ) -> std::optional< int > {
	if ( const auto candidate = mcode::cli::parse_exec_options( arguments ); !candidate ) {
		std::fprintf( stderr, "mcode: %s\n\n", candidate.error( ).msg.c_str( ) );
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	} else {
		parsed = *candidate;
	}

	if ( !parsed.unknown_arguments.empty( ) ) {
		std::fprintf( stderr, "mcode: unknown argument '%s'\n\n",
			parsed.unknown_arguments.front( ).c_str( ) );
		std::fputs( mcode::cli::usage_text( "mcode" ).c_str( ), stderr );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	// A listing must succeed unconfigured, so it is answered before any config load.
	if ( parsed.list_sessions ) {
		return mcode::cli::to_int( mcode::cli::print_sessions( invocation_root( parsed ) ) );
	}

	// A mistyped id is an argument error, so it is reported as one rather than as a
	// failure to reach the provider.
	if ( !parsed.resume_session.empty( ) || parsed.continue_session ) {
		const auto existing = mcode::cli::resolve_session( invocation_root( parsed ),
			parsed.resume_session );

		if ( !existing ) {
			std::fprintf( stderr, "mcode: %s\n", existing.error( ).msg.c_str( ) );

			return mcode::cli::to_int(
				mcode::cli::exit_code_for( existing.error( ).code ) );
		}
	}

	return std::nullopt;
}

auto run_exec( const std::vector< std::string >& arguments ) -> int {
	auto parsed = mcode::cli::exec_options{ };

	if ( const auto refused = refuse_bad_invocation( arguments, parsed ); refused ) {
		return *refused;
	}

	auto stream = mcode::cli::json_stream{ parsed.json };
	stream.emit_run_start( parsed.prompt );

	const auto config = mcode::config::load( parsed.working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed.working_directory } );

	if ( !config ) {
		std::fprintf( stderr, "mcode: %s\n", config.error( ).msg.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, config.error( ).msg );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	}

	const auto provider_name = parsed.model.empty( )
		? config->get_string( "model.provider" ).value_or( std::string{ } )
		: parsed.model;
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

	const auto workspace_path = parsed.working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed.working_directory };

	// Created before anything resolves the root, so every path the run touches points into it.
	auto effective_workspace = workspace_path;

	if ( parsed.worktree ) {
		auto created = mcode::cli::create_worktree( workspace_path, parsed.worktree_name );

		if ( !created ) {
			return mcode::cli::detail::report_failure( stream, created.error( ) );
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

	if ( parsed.verbose ) {
		for ( const auto& warning : skills_context.warnings ) {
			std::fprintf( stderr, "mcode: %s\n", warning.c_str( ) );
		}
	}

	// Must outlive the loop: the registered tools close over it.
	auto extensions = mcode::ext::load_result{ };

	// Read by the MCP connect step after the loader has run.
	auto extension_servers = mcode::ext::mcp_server_store{ };

	// The loader's `fs.*` surface entries use this same context.
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

	// Declared here so the extension surface's session callbacks can read it; it is
	// opened after the extensions load.
	auto log = mcode::event_log{ };

	auto commands = mcode::ext::command_registry{ };

	auto budget = mcode::session_budget{ };

	if ( parsed.max_steps > 0 ) {
		budget.max_steps = parsed.max_steps;
	}

	if ( parsed.max_budget_usd > 0.0 ) {
		budget.max_usd = parsed.max_budget_usd;
	}

	// The surface holds this address from load time, so the struct is filled before
	// the load even though some of the values it needs are read later.
	auto tools_context = mcode::tools::tool_context{ };

	// The HTTP client the model and every network collaborator share. Declared here,
	// ahead of the extension load, because the collaborators capture it by reference.
	auto transport = mcode::net::http_client{ };

	// One struct shared with the interactive path, so a capability added there
	// reaches here too rather than only one of the two. Built before the load, since
	// the surface holds these addresses from load time.
	auto collaborators = mcode::cli::build_collaborators(
		{ .tools = &tools_context, .transport = &transport, .permissions = &engine,
			.config = &*config, .log = &log, .budget = &budget });

	if ( !parsed.no_extensions ) {
		auto options = mcode::ext::loader_options{ };

		auto context = mcode::ext::register_context{ .skills = &skills_context.skills,
			.servers = &extension_servers, .config = &*config, .space = &*space,
			.reads = &reads, .permissions = &engine, .commands = &commands };

		collaborators.apply( context );

		// The skills outlive the extensions, which resolve `skill_read` against them.
		options.register_api = mcode::ext::default_register_api( tool_registry,
			context );

		auto roots = mcode::ext::default_roots( effective_workspace );

		if ( bundled_directory ) {
			roots.push_back( *bundled_directory / "extensions" );
		}

		extensions = mcode::ext::load_workspace_extensions( effective_workspace, roots,
			registry, hooks, options );

		if ( parsed.verbose ) {
			for ( const auto& failure : extensions.report.failed ) {
				std::fprintf( stderr, "mcode: extension '%s' failed to load: %s\n",
					failure.name.c_str( ), failure.reason.c_str( ) );
			}

			for ( const auto& inert : extensions.report.inert ) {
				std::fprintf( stderr, "mcode: extension '%s' is inert: %s\n",
					inert.name.c_str( ), inert.detail.c_str( ) );
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

	auto client = mcode::model::http_model_client{ transport };
	auto loop_client = mcode::cli::detail::streaming_client{ client, stdout, parsed.json };

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
	const auto interactive = !parsed.json && mcode::platform::terminal_size( ).has_value( );

	if ( parsed.verbose ) {
		std::fprintf( stderr, "mcode: sandbox: %s (filesystem %s, network %s)\n",
			mcode::platform::sandbox_mechanism( ).data( ),
			mcode::platform::to_string( mcode::platform::sandbox_capability_level( ) ).data( ),
			mcode::platform::to_string( mcode::platform::sandbox_network_level( ) ).data( ) );
	}

	{
		auto engine_options = mcode::cli::detail::resolve_approval_policy( parsed, *config, interactive );

		engine.set_options( engine_options );
		engine.set_approval_source( interactive
			? static_cast< mcode::perm::approval_source* >( &terminal_source )
			: static_cast< mcode::perm::approval_source* >( &headless_source ) );

		for ( const auto& dir : parsed.add_dirs ) {
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

	tools_context.space = &*space;
	tools_context.reads = &reads;
	tools_context.permissions = &engine;
	tools_context.run_id = run_id;
	tools_context.headless = parsed.json;

	// Only when `[egress] allow` names a host; absent means the child gets no network at all.
	auto egress = mcode::cli::start_egress_if_configured( *config );
	tools_context.egress = egress.get( );

	auto sink = mcode::tools::vector_sink{ };

	const auto registered = mcode::tools::register_core_tools( tool_registry, sink,
		tools_context );

	if ( !registered ) {
		return mcode::cli::detail::report_failure( stream, registered.error( ) );
	}

	auto provider = *descriptor;

	if ( base_url ) {
		provider.endpoint = *base_url;
	}

	// Opened first, so every event from the first step lands on disk.
	const auto session = mcode::cli::open_session_for_run( parsed, space->root( ) );

	if ( !session ) {
		return mcode::cli::detail::report_failure( stream, session.error( ) );
	}

	const auto resumed = !parsed.resume_session.empty( ) || parsed.continue_session;

	if ( const auto opened = log.open( session->path ); !opened ) {
		return mcode::cli::detail::report_failure( stream, opened.error( ) );
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

	// Unset tiers are the common case: routing falls back to `model.model` per call.
	if ( const auto tiers = mcode::cli::resolve_tiers( model_name, *config ); !tiers ) {
		const auto message = tiers.error( ).msg;

		std::fprintf( stderr, "mcode: %s\n", message.c_str( ) );
		stream.emit_run_end( mcode::cli::exit_code::usage_error, message );

		return mcode::cli::to_int( mcode::cli::exit_code::usage_error );
	} else {
		dependencies.plan_tier = tiers->plan;
		dependencies.act_tier = tiers->act;
		dependencies.side_tier = tiers->side;
	}

	dependencies.provider = provider;
	dependencies.api_key = *api_key;
	dependencies.workspace_root = space->root( ).string( );
	dependencies.platform_name = std::string{ mcode::cli::PLATFORM_NAME };
	dependencies.permissions = &engine;

	dependencies.instruction_chain = skills_context.chain.text;
	dependencies.skill_index = skills_context.skill_index;

	// A headless run edits files too, so it needs the same undo store the interactive path has.
	std::optional< mcode::snapshot_store > snapshots;

	if ( auto data = mcode::platform::app_data_path( mcode::platform::data_kind::data ) ) {
		snapshots.emplace( *data / mcode::cli::detail::SNAPSHOT_DIRECTORY );
		dependencies.snapshots = &*snapshots;
	}

	// The loop drives the timers: one pump per step, on the thread the VM belongs to.
	dependencies.pump_timers = [ &extensions ]( ) { extensions.pump_timers( ); };
	dependencies.pump_deferred = [ &extensions ]( ) { extensions.pump_deferred( ); };

	// Must outlive the loop: the supervisors own the child processes the loop dispatches into.
	auto mcp_servers = mcode::mcp::server_set{ };

	auto loop = mcode::agent_loop{ dependencies };

	mcode::cli::set_active_loop( &loop );

	// Armed from configuration, never from the model's own output.
	{
		auto command = parsed.verify_command;

		if ( command.empty( ) ) {
			command = config->get_string( "verify.command" ).value_or( std::string{ } );
		}

		if ( !command.empty( ) ) {
			loop.set_verification_command( command );
		}
	}

	// Seeded first, so the model continues instead of starting from empty.
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
		return mcode::cli::detail::report_failure( stream, connected.error( ) );
	}

	const auto outcome = loop.run( parsed.prompt );

	if ( !outcome ) {
		// Separated from the streamed answer above it, which a failure may have cut mid-line.
		std::fputc( '\n', stderr );

		return mcode::cli::detail::report_failure( stream, outcome.error( ) );
	}

	const auto code = mcode::cli::exit_code_for_run( *outcome, loop.budget( ).exhausted( ),
		loop.permission_denied( ) );

	if ( parsed.verbose ) {
		const auto usage = client.accumulated_usage( );
		const auto cost = mcode::model::compute_cost( *caps, usage );

		std::fprintf( stderr, "mcode: %d steps, %lld in, %lld out, $%.4f\n",
			static_cast< int >( outcome->steps ),
			static_cast< long long >( usage.input ), static_cast< long long >( usage.output ),
			cost );
	}

	return mcode::cli::detail::report_run_outcome( stream, loop, *outcome, code,
		parsed.json, parsed.verbose );
}
