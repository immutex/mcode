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
#include "mcode/support/json.hxx"
#include "mcode/support/time.hxx"
#include "mcode/tools/context.hxx"
#include "mcode/tools/register.hxx"
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

#include "cli_session.hxx"

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

		// Reads `std::cin`, correct in plain mode which never enters raw mode.
		mcode::perm::terminal_approval_source terminal_source;

		session_parts( ) : hooks( bus ), client( transport ) { }
	};

	static auto parts = std::optional< session_parts >{ };

	// A second call would build an unwired loop over shared parts, so it is refused.
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

	// Built before the loader so the extension surface's `fs.*` entries share this context.
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

		// The engine's null-source path denies every ask, which would refuse every tool call.
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
	parts->loop_deps.platform_name = std::string{ mcode::cli::PLATFORM_NAME };
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
