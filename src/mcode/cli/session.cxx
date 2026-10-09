#include "mcode/agent/loop.hxx"
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
#include "exec_internal.hxx"

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

#include "mcode/cli/session.hxx"

namespace {

	// The load report of the session that was built, so `/extensions` can read it
	// without the loop carrying a pointer it has no other use for -- the loop
	// knows four abstractions and extensions are not one of them. The report
	// lives on the session's own g_parts, which are function-local and outlive the
	// returned loop; this points at them. Null until a session is built.
	const mcode::ext::load_report* g_session_extensions = nullptr;

	// Everything one interactive session owns, at file scope rather than inside
	// the builder, because three more entry points need it: `/extensions` reads
	// the load report and `/resume`, `/continue` and `/new` repoint the log. It
	// was a function-local static, which meant the only way to reach any of it was
	// through the function that built it.
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

		// The extension surface borrows `skills`, and the surfaces live on in
		// `extensions` and are invoked on every later turn -- so the report has to
		// outlive the builder. It was a plain local, and every other field that
		// must outlive the returned loop was already parked here for that reason.
		mcode::skills::session_context skills_context;

		mcode::net::http_client transport;
		mcode::model::http_model_client client;
		mcode::event_log log;

		// Undo is what makes the permissive approval default survivable, so the store is
		// always built: /undo has something to restore rather than reporting a zero.
		std::optional< mcode::snapshot_store > snapshots;
		mcode::session_budget budget;
		mcode::agent_loop::dependencies loop_deps;

		// Reads `std::cin`, correct in plain mode which never enters raw mode.
		mcode::perm::terminal_approval_source terminal_source;

		session_parts( ) : hooks( bus ), client( transport ) { }
	};

	// One session per process: a second build would make an unwired loop over
	// these shared parts.
	auto g_parts = std::optional< session_parts >{ };

	// Releases the session slot unless the build reached its success return.
	//
	// The parts are emplaced before anything is validated, because the validation
	// reads them. Every failure after that used to leave them engaged, so the next
	// call reported "an interactive session is already running in this process" --
	// the wrong diagnosis for a retry after fixing a config.
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

		// The build succeeded; the parts live for the rest of the process.
		auto release( ) noexcept -> void { held_ = false; }

	private:
		bool held_ = true;
	};

}

auto build_interactive_loop( const mcode::cli::exec_options& parsed,
	mcode::perm::approval_source* interactive_approval )
	-> mcode::result< mcode::agent_loop > {
	// A second call would build an unwired loop over shared g_parts, so it is refused.
	if ( g_parts ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"an interactive session is already running in this process" ) );
	}

	g_parts.emplace( );

	auto reservation = parts_reservation{ };

	const auto workspace_path = parsed.working_directory.empty( )
		? std::filesystem::current_path( )
		: std::filesystem::path{ parsed.working_directory };

	// Resolved before any provider or key work, so a mistyped --resume id is reported as
	// the argument error it is rather than as a provider complaint. The loop opens the
	// same file again below; this only fails the run early and by name.
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

	// Built before the loader so the extension surface's `fs.*` entries share this context.
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
		auto options = mcode::ext::loader_options{ };
		options.register_api = mcode::ext::default_register_api( g_parts->tools,
			{ .skills = &skills_context.skills, .servers = &g_parts->extension_servers,
				.config = &*config, .space = &*g_parts->space, .reads = &g_parts->reads,
				.permissions = &*g_parts->engine } );

		auto roots = mcode::ext::default_roots( workspace_path );

		if ( bundled_directory ) {
			roots.push_back( *bundled_directory / "extensions" );
		}

		g_parts->extensions = mcode::ext::load_extensions( roots, g_parts->providers,
			g_parts->hooks, options );
	}

	// Published whether or not extensions were loaded: with `--no-extensions` the
	// report is empty, and `/extensions` saying "0 loaded" is the true answer
	// rather than a null it has to explain.
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

	auto tools_context = mcode::tools::tool_context{ };
	tools_context.space = &*g_parts->space;
	tools_context.reads = &g_parts->reads;
	tools_context.permissions = &*g_parts->engine;
	tools_context.run_id = run_id;
	tools_context.headless = parsed.json;

	auto sink = mcode::tools::vector_sink{ };

	const auto registered = mcode::tools::register_core_tools( g_parts->tools, sink,
		tools_context );

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
	g_parts->loop_deps.provider_name = provider_name;
	g_parts->loop_deps.provider = provider;
	g_parts->loop_deps.api_key = *api_key;
	g_parts->loop_deps.workspace_root = g_parts->space->root( ).string( );
	g_parts->loop_deps.platform_name = std::string{ mcode::cli::PLATFORM_NAME };
	g_parts->loop_deps.permissions = &*g_parts->engine;
	g_parts->loop_deps.instruction_chain = skills_context.chain.text;
	g_parts->loop_deps.skill_index = skills_context.skill_index;

	// The store lives in the per-user data directory, keyed by nothing else: the index
	// records the workspace-relative path, so one store serves every project.
	if ( auto data = mcode::platform::app_data_path( mcode::platform::data_kind::data ) ) {
		g_parts->snapshots.emplace( *data / mcode::cli::detail::SNAPSHOT_DIRECTORY );
		g_parts->loop_deps.snapshots = &*g_parts->snapshots;
	}

	// the loop drives the timers: one pump per step, on the thread the VM belongs to.
	g_parts->loop_deps.pump_timers = [ ]( ) { g_parts->extensions.pump_timers( ); };

	auto loop = mcode::agent_loop{ g_parts->loop_deps };

	// The session file is opened before the first turn, so an interactive session's
	// events are on disk the same way an exec run's are. An unknown --resume id is
	// refused here rather than starting a fresh session under it.
	const auto session = mcode::cli::open_session_for_run( parsed, g_parts->space->root( ) );

	if ( !session ) {
		return std::unexpected( session.error( ) );
	}

	const auto resumed = !parsed.resume_session.empty( ) || parsed.continue_session;

	if ( const auto opened = g_parts->log.open( session->path ); !opened ) {
		return std::unexpected( opened.error( ) );
	}

	std::fputs( mcode::cli::start_session_log( g_parts->log, *session, resumed ).c_str( ), stderr );

	// A resumed session continues its conversation. The headless path has always
	// rebuilt the transcript here; the interactive one opened the log, reported
	// how many messages it held, and then left `history_` empty -- so
	// `--continue` in the TUI restored the file and forgot the conversation, and
	// the model started from nothing while the report said it had carried N
	// messages. Seeded before the first turn, exactly as `run_exec` does it.
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

	// The build succeeded, so the parts stay engaged for the life of the session.
	reservation.release( );

	return loop;
}

auto session_extensions( ) -> const mcode::ext::load_report* {
	return g_session_extensions;
}

auto adopt_session( mcode::agent_loop& loop, const std::string_view id )
	-> mcode::result< std::string > {
	if ( !g_parts ) {
		return std::unexpected( mcode::fail( mcode::errc::config,
			"no interactive session is running" ) );
	}

	// An unknown id is a named error from here and never falls back to a fresh
	// session; an empty one is the workspace's newest, which is `/continue`.
	auto session = mcode::cli::resolve_session( g_parts->space->root( ), id );

	if ( !session ) {
		return std::unexpected( session.error( ) );
	}

	// Repoint first: `restore_transcript` reads the log's own events, so it must
	// see the new file and nothing else. `event_log::open` clears the previous
	// session's events, which is what stops the two transcripts merging.
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

	// An empty history, not the previous session's: this is a new conversation.
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
