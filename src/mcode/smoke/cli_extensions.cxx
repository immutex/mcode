#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "mcode/agent/subagent.hxx"
#include "mcode/cli/exec.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/core/version.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/api_cmd.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/ext/lua_host.hxx"
#include "mcode/model/provider.hxx"
#include "mcode/platform/seams.hxx"

#include "mcode/smoke/check.hxx"

using smoke::check;
using smoke::g_failures;
using smoke::section;

namespace {

	// From the running executable, not a compiled-in path: the source tree is not
	// there for a released binary.
	[[nodiscard]] auto bundled_extensions_directory( ) -> std::filesystem::path {
		const auto executable = mcode::platform::executable_directory( );

		return executable ? *executable / "extensions" : std::filesystem::path{ };
	}

	// Counted, not pinned: a shipped extension that is neither loaded nor failed still trips this.
	[[nodiscard]] auto shipped_extension_count( const std::filesystem::path& directory )
		-> std::size_t {
		auto error = std::error_code{ };
		const auto entries = std::filesystem::directory_iterator{ directory, error };

		if ( error ) {
			return 0;
		}

		auto count = std::size_t{ 0 };

		for ( const auto& entry : entries ) {
			if ( std::filesystem::exists( entry.path( ) / "ext.toml", error ) && !error ) {
				++count;
			}

			error.clear( );
		}

		return count;
	}

}

auto smoke_cli_and_extensions( ) -> void {
	section( "cli (headless surface)" );

	{
		check( mcode::cli::to_int( mcode::cli::exit_code::success ) == 0, "exit 0 is success" );
		check( mcode::cli::to_int( mcode::cli::exit_code::usage_error ) == 2,
			"exit 2 is a usage error" );
		check( mcode::cli::to_int( mcode::cli::exit_code::interrupted ) == 130,
			"exit 130 is interrupted" );

		// An unrecognised flag has no arity, so it does not consume the following token.
		auto parsed = mcode::cli::parse_exec_options(
			{ "--json", "--model", "m", "do the thing", "--max-step" } );

		check( static_cast< bool >( parsed ), "parsed a valid command line" );

		if ( parsed ) {
			check( parsed->json, "--json was recognised" );
			check( parsed->model == "m", "--model took its value" );
			check( parsed->prompt == "do the thing", "the bare argument became the prompt" );
			check( parsed->unknown_arguments.size( ) == 1,
				"the typo --max-step was collected rather than ignored" );
			check( parsed->unknown_arguments.front( ) == "--max-step",
				"the collected argument is the typo itself" );
		}

		auto arity = mcode::cli::parse_exec_options( { "--max-step", "5", "prompt" } );

		check( static_cast< bool >( arity ), "parsed with an unknown flag present" );

		if ( arity ) {
			check( arity->prompt == "5", "the unknown flag did not consume its neighbour" );
			check( arity->unknown_arguments.size( ) == 2,
				"both the typo and the extra bare argument were collected" );
		}

		auto missing = mcode::cli::parse_exec_options( { "--model" } );
		check( !missing, "a flag without a value is refused" );

		// A single-dash typo is as invisible as a double-dash one: collect it, never send it as the prompt.
		auto short_flag = mcode::cli::parse_exec_options( { "-x" } );

		check( static_cast< bool >( short_flag ), "parsed with an unknown short flag present" );

		if ( short_flag ) {
			check( short_flag->prompt.empty( ), "the short flag did not become the prompt" );
			check( short_flag->unknown_arguments.size( ) == 1,
				"the unknown short flag was collected" );
			check( short_flag->unknown_arguments.front( ) == "-x",
				"the collected argument is the short flag itself" );
		}

		auto short_with_prompt = mcode::cli::parse_exec_options( { "-x", "do it" } );

		check( static_cast< bool >( short_with_prompt ), "parsed the short flag and a prompt" );

		if ( short_with_prompt ) {
			check( short_with_prompt->prompt == "do it", "the prompt still landed" );
			check( short_with_prompt->unknown_arguments.size( ) == 1,
				"only the short flag was collected" );
		}

		auto bad_number = mcode::cli::parse_exec_options( { "--max-steps", "lots" } );
		check( !bad_number, "a non-numeric flag value is refused" );

		auto stream = mcode::cli::json_stream{ true };
		stream.emit_run_start( "prompt" );

		auto event = mcode::events::event{ };
		event.type = mcode::events::kind::tool_call;
		event.payload_json = R"({"name":"read"})";
		stream.emit_event( event );

		stream.emit_run_end( mcode::cli::exit_code::success, "done" );
		stream.emit_run_end( mcode::cli::exit_code::provider_error, "second" );

		check( stream.run_end_emitted( ), "run.end was emitted" );
		check( stream.lines_emitted( ) == 3, "run.start, one event, and exactly one run.end" );
	}
	// The fixture tree exists only for a source-tree build; the shipped extensions
	// below are checked either way.
	const auto fixture_tree_present =
		std::filesystem::exists( std::filesystem::path{ MCODE_SMOKE_EXTENSIONS } );

	if ( !fixture_tree_present ) {
		std::printf( "  (fixture extensions not present; skipping the fixture loader checks)\n" );
	}

	section( "extension loader" );

	if ( fixture_tree_present ) {
		auto registry = mcode::tool_registry{ };
		auto providers = mcode::model::provider_registry{ };
		auto bus = mcode::events::bus{ };
		auto hooks = mcode::ext::hook_registry{ bus };

		const auto extensions = std::filesystem::path{ MCODE_SMOKE_EXTENSIONS };

		auto options = mcode::ext::loader_options{ };
		options.register_api = mcode::ext::default_register_api( registry );

		auto loaded = mcode::ext::load_extensions( { extensions }, providers, hooks, options );

		for ( const auto& failure : loaded.report.failed ) {
			std::printf( "  loader: %s failed: %s\n", failure.name.c_str( ),
				failure.reason.c_str( ) );
		}

		check( loaded.report.loaded.size( ) == 1, "the valid fixture extension loaded" );
		check( loaded.report.failed.size( ) == 1, "the invalid fixture extension failed" );

		// Guarded: an unguarded `front()` on an empty result is an access violation
		// that truncates every check after it.
		if ( !loaded.report.loaded.empty( ) ) {
			check( loaded.report.loaded.front( ).name == "hello-tool",
				"the loaded extension is the valid one" );
		}

		const auto* hello = registry.find( "hello" );
		check( hello != nullptr, "the extension's tool is in the registry" );

		if ( hello != nullptr ) {
			check( hello->owner == "hello-tool", "the tool is attributed to its extension" );
			check( !hello->is_core( ), "the tool is not core" );
		}

		auto greeting = loaded.invoke( "hello", R"({"name":"smoke"})" );
		check( greeting && greeting->find( "hello from hello-tool to smoke" ) != std::string::npos,
			"the tool ran and returned the extension's answer",
			greeting ? *greeting : greeting.error( ).msg );

		auto refused = loaded.invoke( "hello", "{}" );
		check( !refused && refused.error( ).msg == "name is required",
			"an environmental failure returns the extension's message",
			refused ? std::string{ "unexpectedly succeeded" } : refused.error( ).msg );

		section( "extension hooks (a hook written in Luau)" );

		check( hooks.handlers_for( "tool.pre_call" ) == 1, "the extension's hook is subscribed" );
		check( hooks.handlers_for( "hello-tool.ready" ) == 1, "its custom event is subscribed" );

		auto allowed = mcode::events::event{ };
		allowed.type = mcode::events::kind::tool_pre_call;
		allowed.payload_json = R"({"name":"read"})";
		check( !bus.publish( allowed ), "an unobjectionable tool call passes the hook" );

		auto blocked = mcode::events::event{ };
		blocked.type = mcode::events::kind::tool_pre_call;
		blocked.payload_json = R"({"name":"forbidden"})";

		auto veto = bus.publish( blocked );
		check( veto && veto->reason == "blocked by hello-tool" && veto->source == "hello-tool",
			"the hook vetoed the call, attributed to the extension",
			veto ? veto->reason : std::string{ "no veto" } );

		if ( !loaded.extensions.empty( ) && loaded.extensions.front( ).host != nullptr ) {
			check( static_cast< bool >( hooks.emit( "hello-tool.ready", R"({"count":7})" ) ),
				"a custom event reaches the extension" );
		}

		check( hooks.total_failures( ) == 0, "no hook threw" );

		section( "extension loader (disable all)" );

		auto bare_registry = mcode::tool_registry{ };

		for ( const auto* name : { "read", "write", "edit", "glob", "grep", "bash" } ) {
			auto core = mcode::tool_def{ };
			core.name = name;
			core.source = mcode::tool_source::core;

			if ( !bare_registry.add( std::move( core ) ) ) {
				check( false, "could not build the core registry" );

				break;
			}
		}

		auto bare_providers = mcode::model::provider_registry{ };
		auto bare_bus = mcode::events::bus{ };
		auto bare_hooks = mcode::ext::hook_registry{ bare_bus };

		auto disabled_options = mcode::ext::loader_options{ };
		disabled_options.disabled = true;
		disabled_options.register_api = mcode::ext::default_register_api( bare_registry );

		auto bare = mcode::ext::load_extensions( { extensions }, bare_providers, bare_hooks,
			disabled_options );

		check( bare.report.loaded.empty( ), "nothing loaded when disabled" );
		check( bare.report.failed.empty( ), "disabling is not a failure" );
		check( bare.report.disabled == 2, "both extensions were counted as disabled" );
		check( bare_registry.size( ) == 6, "the core tools survived" );
		check( bare_registry.find( "read" ) != nullptr, "a core tool is still present" );
		check( bare_registry.find( "hello" ) == nullptr, "the extension tool is absent" );
		check( bare_providers.empty( ), "no provider was declared" );

		auto absent = bare.invoke( "hello", "{}" );
		check( !absent, "invoking a disabled tool is an error" );

	}

	section( "extension loader (shipped extensions)" );

	auto shipped_registry = mcode::tool_registry{ };
	auto shipped_providers = mcode::model::provider_registry{ };
	auto shipped_bus = mcode::events::bus{ };
	auto shipped_hooks = mcode::ext::hook_registry{ shipped_bus };

	auto shipped_options = mcode::ext::loader_options{ };
	shipped_options.register_api = mcode::ext::default_register_api( shipped_registry );

	auto shipped = mcode::ext::load_extensions(
		{ bundled_extensions_directory( ) }, shipped_providers, shipped_hooks,
		shipped_options );

	for ( const auto& failure : shipped.report.failed ) {
		std::printf( "  loader: %s failed: %s\n", failure.name.c_str( ),
			failure.reason.c_str( ) );
	}

	const auto expected = shipped_extension_count( bundled_extensions_directory( ) );

	check( expected > 0, "the bundled extensions directory is present" );
	check( shipped.report.loaded.size( ) == expected,
		"every shipped extension directory loaded" );
	check( shipped.report.failed.empty( ), "every shipped extension loaded" );

	// Without this extension the binary has no provider and cannot run a single turn.
	check( shipped_providers.size( ) == 3, "the providers extension declared three providers" );
	check( shipped_providers.find( "openai-chat-completions" ) != nullptr,
		"the OpenAI descriptor is registered" );
	check( shipped_registry.find( "skill_read" ) != nullptr,
		"the skills extension registered skill_read" );
	check( shipped_registry.find( "memory_search" ) != nullptr,
		"the memory extension registered memory_search" );

	// The subagent is read-only: if `bash` appears in its tool set the capability boundary is a prompt again.
	check( mcode::subagent_tool_names( ).size( ) == 3,
		"the read-only subagent has exactly three tools" );

	for ( const auto name : mcode::subagent_tool_names( ) ) {
		check( name != "bash" && name != "edit" && name != "write",
			"no mutating tool reaches a read-only subagent" );
	}

	section( "extension loader (shipped extensions, collaborators installed)" );

	// The load above installed no collaborators, so a bundled extension whose tool
	// needs one is present and inert. This is the other half: with a surface wired
	// the way the session wires it, every bundled tool must actually register.
	// A load that succeeds without registering is the defect this exists to catch.
	auto wired_registry = mcode::tool_registry{ };
	auto wired_providers = mcode::model::provider_registry{ };
	auto wired_bus = mcode::events::bus{ };
	auto wired_hooks = mcode::ext::hook_registry{ wired_bus };

	auto wired_commands = mcode::ext::command_registry{ };
	auto wired_session_state = std::function< mcode::ext::session_state( ) >{
		[ ]( ) { return mcode::ext::session_state{ }; } };
	auto wired_plan = std::function< void( std::string_view ) >{ []( std::string_view ) { } };
	auto wired_bash = std::function< mcode::result< std::string >(
		const std::string&, std::int64_t ) >{
		[ ]( const std::string&, std::int64_t ) -> mcode::result< std::string > {
			return std::string{ "{\"ok\":true,\"succeeded\":true,\"exit_code\":0}" };
		} };
	auto wired_task = std::function< mcode::result< mcode::subagent_report >(
		const mcode::subagent_spec& ) >{
		[ ]( const mcode::subagent_spec& ) -> mcode::result< mcode::subagent_report > {
			return mcode::subagent_report{ .summary = "a report", .steps = 1 };
		} };
	auto wired_search = std::function< mcode::ext::search_result( const std::string& ) >{
		[ ]( const std::string& ) -> mcode::ext::search_result {
			return std::vector< mcode::ext::search_hit >{ };
		} };
	auto wired_fetch = std::function< mcode::result< mcode::ext::fetch_result >(
		const std::string&, std::int64_t ) >{
		[ ]( const std::string&, std::int64_t ) -> mcode::result< mcode::ext::fetch_result > {
			return mcode::ext::fetch_result{ .status = 200, .content_type = "text/plain",
				.body = "hello" };
		} };

	auto wired_options = mcode::ext::loader_options{ };
	wired_options.register_api = mcode::ext::default_register_api( wired_registry,
		{ .commands = &wired_commands, .session_state = &wired_session_state,
			.web_searcher = &wired_search, .subagent_runner = &wired_task,
			.bash_runner = &wired_bash, .plan_sink = &wired_plan,
			.fetch_runner = &wired_fetch } );

	auto wired = mcode::ext::load_extensions(
		{ bundled_extensions_directory( ) }, wired_providers, wired_hooks, wired_options );

	for ( const auto& failure : wired.report.failed ) {
		std::printf( "  loader: %s failed: %s\n", failure.name.c_str( ),
			failure.reason.c_str( ) );
	}

	check( wired.report.failed.empty( ),
		"every shipped extension loaded with its collaborators installed" );

	// Each of these needs a collaborator, so each is absent from the earlier load.
	for ( const auto name : { "task", "todo", "web_search", "fetch", "git_status",
			"git_diff", "git_commit", "gh_pr", "gh_issue", "gh_review" } ) {
		check( wired_registry.find( name ) != nullptr,
			std::string{ "the " } + name + " tool registered" );
	}

	// The git and GitHub extensions are gated on `spawn`, not `read`: a tool that
	// can run a command must not auto-approve inside the workspace.
	for ( const auto name : { "git_commit", "gh_pr" } ) {
		const auto* definition = wired_registry.find( name );

		check( definition != nullptr && definition->klass == mcode::tool_class::exec,
			std::string{ name } + " is an exec-class tool" );
	}

	// Registration is not capability. An extension registers its tools whenever its
	// `init.luau` runs, and the `mcode.X` table entry exists whether or not a
	// collaborator is behind it, so a surface that lost one passes every check above
	// and fails only at the call site. These assert the accessors directly.
	{
		auto host = mcode::lua_host::create( );

		if ( !host ) {
			check( false, "a host can be created for the capability probe" );

			return;
		}

		auto surface = mcode::ext::api_surface{ };
		const auto details = mcode::ext::manifest{ };

		if ( const auto installed = surface.install( mcode::ext::api_surface::install_request{
				.host = *host, .registry = wired_registry, .providers = wired_providers,
				.hooks = wired_hooks, .details = details, .commands = &wired_commands,
				.session_state = &wired_session_state, .web_searcher = &wired_search,
				.subagent_runner = &wired_task, .bash_runner = &wired_bash,
				.plan_sink = &wired_plan, .fetch_runner = &wired_fetch } ); !installed ) {
			check( false, "the probe surface installed" );

			return;
		}

		check( surface.subagent_runner( ) != nullptr,
			"install copied the subagent runner out of the request" );
		check( surface.bash_runner( ) != nullptr,
			"install copied the command runner out of the request" );
		check( surface.plan_sink( ) != nullptr,
			"install copied the plan sink out of the request" );
		check( surface.fetch_runner( ) != nullptr,
			"install copied the fetch runner out of the request" );
	}

	if ( const auto* descriptor = shipped_providers.find( "anthropic-messages" );
		descriptor != nullptr ) {
		check( descriptor->endpoint.starts_with( "https://" ),
			"the descriptor carries a real endpoint" );
	}
}
