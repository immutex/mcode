#include "exec_internal.hxx"
#include "mcode/agent/loop.hxx"
#include "mcode/agent/subagent.hxx"
#include "mcode/cli/active_loop.hxx"
#include "mcode/cli/egress_setup.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/cli/repl.hxx"
#include "mcode/cli/session.hxx"
#include "mcode/cli/session_state.hxx"
#include "mcode/cli/slash.hxx"
#include "mcode/cli/tier_config.hxx"
#include "mcode/cli/extension_wiring.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/api_cmd.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/fs/snapshot.hxx"
#include "mcode/fs/workspace.hxx"
#include "mcode/mcp/connect.hxx"
#include "mcode/memory/ledger.hxx"
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
#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/tools/exec_tools.hxx"
#include "mcode/tools/register.hxx"
#include "mcode/tools/tool_args.hxx"
#include "mcode/tui/approval_tui.hxx"
#include "mcode/tui/editor.hxx"
#include "mcode/tui/frame.hxx"
#include "mcode/tui/render.hxx"
#include "mcode/tui/tty.hxx"

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

namespace {

	// Points at the session's own report, which outlives the returned loop.
	const mcode::ext::load_report* g_session_extensions = nullptr;

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

		// Held here, not passed as a temporary: the surface copies the function but the
		// pointer into it must outlive the load.
		mcode::ext::command_registry commands;
		std::function< void( const std::string&, const std::string& ) > instruction_sink_fn;

		// One struct, so a capability added here reaches both paths.
		mcode::cli::extension_collaborators collaborators;

		// Builtins plus the registered extension commands, rebuilt after the load.
		std::vector< mcode::tui::slash_command > command_rows;

		// Must outlive the returned loop: the extension surface borrows it.
		mcode::skills::session_context skills_context;

		mcode::net::http_client transport;
		mcode::model::http_model_client client;
		mcode::event_log log;

		// Filled after the permission rules load, but the struct itself is stable, so
		// the extension surface can hold its address from load time.
		mcode::tools::tool_context tools_context;

		// Always built, so /undo has something to restore.
		std::optional< mcode::snapshot_store > snapshots;
		mcode::session_budget budget;
		mcode::agent_loop::dependencies loop_deps;

		// Null when no `[egress] allowlist` was configured, which means no network.
		std::unique_ptr< mcode::net::egress_proxy > egress;

		mcode::perm::terminal_approval_source terminal_source;

		session_parts( ) : hooks( bus ), client( transport ) { }
	};

	auto g_parts = std::optional< session_parts >{ };

	// Releases the session slot on every failure path; `release` keeps it.
	class parts_reservation {
	public:
		parts_reservation( ) = default;
		parts_reservation( const parts_reservation& ) = delete;
		auto operator=( const parts_reservation& ) -> parts_reservation& = delete;

		~parts_reservation( ) {
			if ( held_ ) {
				g_parts.reset( );
			}
		}

		auto release( ) noexcept -> void { held_ = false; }

	private:
		bool held_ = true;
	};

}

auto build_interactive_loop( const mcode::cli::exec_options& parsed,
	mcode::perm::approval_source* interactive_approval )
	-> mcode::result< mcode::agent_loop > {
	if ( g_parts ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"an interactive session is already running in this process" ) );
	}

	g_parts.emplace( );

	auto reservation = parts_reservation{ };

	const auto workspace_path = parsed.working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed.working_directory };

	// Resolved first, so a mistyped --resume id is a named argument error.
	if ( !parsed.resume_session.empty( ) || parsed.continue_session ) {
		const auto existing = mcode::cli::resolve_session( workspace_path,
			parsed.resume_session );

		if ( !existing ) {
			return std::unexpected( existing.error( ) );
		}
	}

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
			mcode::config::unconfigured_message( "provider" ) ) );
	}

	if ( model_name.empty( ) ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			mcode::config::unconfigured_message( "model" ) ) );
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

	g_parts->skills_context = mcode::skills::assemble_session_context( skills_options );

	auto& skills_context = g_parts->skills_context;

	// Built first, so the extension surface's `fs.*` entries share this context.
	auto space = mcode::workspace::open( workspace_path );

	if ( !space ) {
		return std::unexpected( space.error( ) );
	}

	g_parts->space = std::move( *space );

	const auto user_data = mcode::platform::app_data_path(
		mcode::platform::data_kind::config );

	g_parts->user_store = mcode::perm::remember_store{ user_data
		? *user_data / "permissions.json"
		: std::filesystem::path{ ".mcode/permissions.json" } };

	g_parts->engine = mcode::perm::permission_engine{ *g_parts->space, &*g_parts->user_store };

	if ( !parsed.no_extensions ) {

		// Appends to the chain the loop is built from, so a contribution made at load
		// time is in the prompt. The loop copies the text after the extensions load.
		g_parts->instruction_sink_fn = [ ]( const std::string&,
			const std::string& text ) {
			if ( text.empty( ) ) {
				return;
			}

			auto& chain = g_parts->skills_context.chain.text;

			if ( !chain.empty( ) ) {
				chain += "\n\n";
			}

			chain += text;
		};

		g_parts->collaborators = mcode::cli::build_collaborators(
			{ .tools = &g_parts->tools_context, .transport = &g_parts->transport,
				.permissions = &*g_parts->engine, .config = &*config,
				.log = &g_parts->log, .budget = &g_parts->budget } );

		auto options = mcode::ext::loader_options{ };
		auto context = mcode::ext::register_context{ .skills = &skills_context.skills,
			.servers = &g_parts->extension_servers, .config = &*config,
			.space = &*g_parts->space, .reads = &g_parts->reads,
			.permissions = &*g_parts->engine, .commands = &g_parts->commands,
			.http_client = &g_parts->transport,
			.instruction_sink = &g_parts->instruction_sink_fn };

		g_parts->collaborators.apply( context );

		options.register_api = mcode::ext::default_register_api( g_parts->tools,
			context );

		auto roots = mcode::ext::default_roots( workspace_path );

		if ( bundled_directory ) {
			roots.push_back( *bundled_directory / "extensions" );
		}

		g_parts->extensions = mcode::ext::load_workspace_extensions( workspace_path, roots,
			g_parts->providers, g_parts->hooks, options );
	}

	// Rebuilt either way: with --no-extensions this is the builtin table, so the
	// palette has one source rather than two.
	g_parts->command_rows = mcode::cli::all_commands( g_parts->commands );

	// Published even with --no-extensions: an empty report is the true answer, not a null.
	g_session_extensions = &g_parts->extensions.report;

	const auto* descriptor = g_parts->providers.find( provider_name );

	if ( descriptor == nullptr ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"no provider named '" + provider_name
			+ "' is registered; check [model] provider and the loaded extensions" ) );
	}

	const auto caps = mcode::model::resolve_capabilities( model_name, &*config );

	if ( !caps ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"model '" + model_name + "' has no capabilities; price it in your config under "
			+ mcode::model::capability_config_section( model_name )
			+ ", or use a model the built-in table knows -- refusing to run unpriced" ) );
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

	g_parts->project_store = mcode::perm::remember_store{
		g_parts->space->root( ) / ".mcode" / "permissions.json" };

	const auto interactive = !parsed.json &&
		mcode::platform::terminal_size( ).has_value( );

	{
		auto engine_options = mcode::cli::detail::resolve_approval_policy(
			parsed, *config, interactive );
		g_parts->engine->set_options( engine_options );

		// The engine's null-source path denies every ask, which would refuse every tool call.
		if ( interactive_approval != nullptr ) {
			g_parts->engine->set_approval_source( interactive_approval );
		} else if ( interactive ) {
			g_parts->engine->set_approval_source( &g_parts->terminal_source );
		}

		for ( const auto& dir : parsed.add_dirs ) {
			g_parts->engine->add_root( dir );
		}

		auto deny_rules = config->get_string_array( "permissions.deny" );
		auto ask_rules = config->get_string_array( "permissions.ask" );

		if ( !deny_rules || !ask_rules ) {
			return std::unexpected( !deny_rules ? deny_rules.error( ) : ask_rules.error( ) );
		}

		g_parts->engine->add_config_rules( mcode::perm::rule_scope::user, *deny_rules, *ask_rules );

		if ( const auto loaded = g_parts->engine->load_store( ); !loaded ) {
			std::fprintf( stderr, "mcode: %s\n", loaded.error( ).msg.c_str( ) );
		}

		if ( const auto loaded = g_parts->engine->load_project_store(
			*g_parts->project_store ); !loaded ) {
			std::fprintf( stderr, "mcode: %s\n", loaded.error( ).msg.c_str( ) );
		}
	}

	const auto run_id = std::to_string(
		static_cast< long long >( mcode::support::epoch_milliseconds( ) ) );

	g_parts->tools_context.space = &*g_parts->space;
	g_parts->tools_context.reads = &g_parts->reads;
	g_parts->tools_context.permissions = &*g_parts->engine;
	g_parts->tools_context.run_id = run_id;
	g_parts->tools_context.headless = parsed.json;

	g_parts->egress = mcode::cli::start_egress_if_configured( *config );
	g_parts->tools_context.egress = g_parts->egress.get( );

	auto sink = mcode::tools::vector_sink{ };

	const auto registered = mcode::tools::register_core_tools( g_parts->tools, sink,
		g_parts->tools_context );

	if ( !registered ) {
		return std::unexpected( registered.error( ) );
	}

	if ( parsed.max_steps > 0 ) {
		g_parts->budget.max_steps = parsed.max_steps;
	}

	if ( parsed.max_budget_usd > 0.0 ) {
		g_parts->budget.max_usd = parsed.max_budget_usd;
	}

	auto provider = *descriptor;

	if ( base_url ) {
		provider.endpoint = *base_url;
	}

	g_parts->loop_deps.registry = &g_parts->tools;
	g_parts->loop_deps.client = &g_parts->client;
	g_parts->loop_deps.log = &g_parts->log;
	g_parts->loop_deps.bus = &g_parts->bus;
	g_parts->loop_deps.budget = g_parts->budget;
	g_parts->loop_deps.model_name = model_name;
	g_parts->loop_deps.caps = *caps;

	// Unset tiers are the common case: routing falls back to `model.model` per call.
	if ( const auto tiers = mcode::cli::resolve_tiers( model_name, *config ); !tiers ) {
		return std::unexpected( tiers.error( ) );
	} else {
		g_parts->loop_deps.plan_tier = tiers->plan;
		g_parts->loop_deps.act_tier = tiers->act;
		g_parts->loop_deps.side_tier = tiers->side;
	}

	g_parts->loop_deps.provider = provider;
	g_parts->loop_deps.api_key = *api_key;
	g_parts->loop_deps.workspace_root = g_parts->space->root( ).string( );
	g_parts->loop_deps.platform_name = std::string{ mcode::cli::PLATFORM_NAME };
	g_parts->loop_deps.permissions = &*g_parts->engine;
	g_parts->loop_deps.instruction_chain = skills_context.chain.text;
	g_parts->loop_deps.skill_index = skills_context.skill_index;

	// Keyed by workspace-relative path, so one per-user store serves every project.
	if ( auto data = mcode::platform::app_data_path( mcode::platform::data_kind::data ) ) {
		g_parts->snapshots.emplace( *data / mcode::cli::detail::SNAPSHOT_DIRECTORY );
		g_parts->loop_deps.snapshots = &*g_parts->snapshots;
	}

	// One pump per step, on the thread the VM belongs to.
	g_parts->loop_deps.pump_timers = [ ]( ) { g_parts->extensions.pump_timers( ); };
	g_parts->loop_deps.pump_deferred = [ ]( ) { g_parts->extensions.pump_deferred( ); };

	auto loop = mcode::agent_loop{ g_parts->loop_deps };

	// Armed from configuration, never from model output; empty stays `no-tests`.
	if ( const auto command = config->get_string( "verify.command" ); command && !command->empty( ) ) {
		loop.set_verification_command( *command );
	}

	// Only when the ledger exists: the prompt must never reference a file that is
	// not there, and the extension gates `memory_search` on the same condition.
	if ( const auto ledger = mcode::memory::ledger_path( workspace_path ); ledger ) {
		if ( const auto existing = mcode::memory::read_ledger( workspace_path );
			existing && !existing->empty( ) ) {
			loop.set_notes_pointer( "Dated notes about this project and this user"
				" (corrections, preferences, things tried and why they failed)."
				" Search them with memory_search before repeating an approach you are"
				" unsure about." );
		}
	}

	// Opened first, so an unknown --resume id is refused, not silently replaced.
	const auto session = mcode::cli::open_session_for_run( parsed, g_parts->space->root( ) );

	if ( !session ) {
		return std::unexpected( session.error( ) );
	}

	const auto resumed = !parsed.resume_session.empty( ) || parsed.continue_session;

	if ( const auto opened = g_parts->log.open( session->path ); !opened ) {
		return std::unexpected( opened.error( ) );
	}

	std::fputs( mcode::cli::start_session_log( g_parts->log, *session, resumed ).c_str( ), stderr );

	// Without this the log is opened and the model still starts from nothing.
	if ( resumed ) {
		auto restored = mcode::cli::restore_transcript( g_parts->log );

		if ( !restored.empty( ) ) {
			loop.seed_history( std::move( restored ) );
		}
	}

	for ( auto& [ name, handler ] : sink.take( ) ) {
		loop.register_handler( name, std::move( handler ) );
	}

	for ( const auto& [ name, owner ] : g_parts->extensions.tool_owners ) {
		loop.register_handler( name,
			[ &extensions = g_parts->extensions, tool_name = name ](
				const std::string_view arguments_json ) -> mcode::result< std::string > {
			return extensions.invoke( tool_name, arguments_json );
		} );
	}

	const auto connected = mcode::mcp::connect_servers(
		mcode::mcp::connect_input{ .config_values = &config->keys( ),
			.extension_servers = &g_parts->extension_servers, .registry = &g_parts->tools,
			.loop = &loop, .owned = &g_parts->mcp_servers } );

	if ( !connected ) {
		return std::unexpected( connected.error( ) );
	}

	reservation.release( );

	return loop;
}

auto session_extensions( ) -> const mcode::ext::load_report* {
	return g_session_extensions;
}

auto session_mcp_servers( ) -> const mcode::mcp::server_set* {
	if ( !g_parts.has_value( ) ) {
		return nullptr;
	}

	return &g_parts->mcp_servers;
}

auto session_commands( ) -> const std::vector< mcode::tui::slash_command >* {
	if ( !g_parts.has_value( ) ) {
		return nullptr;
	}

	return &g_parts->command_rows;
}

auto session_invoke_command( const std::string_view name, const std::string_view arguments )
	-> mcode::result< std::string > {
	if ( !g_parts.has_value( ) ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"no session is built, so no extension command can run" ) );
	}

	return g_parts->extensions.invoke_command( g_parts->commands, name, arguments );
}

auto adopt_session( mcode::agent_loop& loop, const std::string_view id )
	-> mcode::result< std::string > {
	if ( !g_parts ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"no interactive session is running" ) );
	}

	// An unknown id is a named error, never a fallback to a fresh session.
	auto session = mcode::cli::resolve_session( g_parts->space->root( ), id );

	if ( !session ) {
		return std::unexpected( session.error( ) );
	}

	// Repoint first: `restore_transcript` reads the log's own events, and `open`
	// clears the previous session's.
	if ( auto opened = g_parts->log.open( session->path ); !opened ) {
		return std::unexpected( opened.error( ) );
	}

	auto restored = mcode::cli::restore_transcript( g_parts->log );
	auto report = mcode::cli::start_session_log( g_parts->log, *session, true );

	loop.reset_session( std::move( restored ) );

	return report;
}

auto start_new_session( mcode::agent_loop& loop ) -> mcode::result< std::string > {
	if ( !g_parts ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"no interactive session is running" ) );
	}

	auto session = mcode::cli::new_session_ref( g_parts->space->root( ) );

	if ( !session ) {
		return std::unexpected( session.error( ) );
	}

	// `open` with a path that does not exist yet creates it.
	if ( auto opened = g_parts->log.open( session->path ); !opened ) {
		return std::unexpected( opened.error( ) );
	}

	auto report = mcode::cli::start_session_log( g_parts->log, *session, false );

	loop.reset_session( { } );

	return report;
}

auto workspace_sessions( ) -> mcode::result< std::vector< mcode::cli::session_ref > > {
	if ( !g_parts ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"no interactive session is running" ) );
	}

	return mcode::cli::list_sessions( g_parts->space->root( ) );
}
