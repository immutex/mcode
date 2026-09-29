// The headless CLI and extension-loader smoke checks. Split from main.cxx, which
// grew past the project's file-length limit; these are the batch's end-to-end
// acceptance checks and they read better with room around them.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "mcode/cli/exec.hxx"
#include "mcode/core/registry.hxx"
#include "mcode/core/version.hxx"
#include "mcode/events/bus.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/ext/lua_host.hxx"
#include "mcode/model/provider.hxx"

#include "smoke.hxx"

using smoke::check;
using smoke::g_failures;
using smoke::section;

auto smoke_cli_and_extensions( ) -> void {
	section( "cli (headless surface)" );

	{
		// Exit codes are an interface a script branches on, so each one is asserted.
		check( mcode::cli::to_int( mcode::cli::exit_code::success ) == 0, "exit 0 is success" );
		check( mcode::cli::to_int( mcode::cli::exit_code::usage_error ) == 2,
			"exit 2 is a usage error" );
		check( mcode::cli::to_int( mcode::cli::exit_code::interrupted ) == 130,
			"exit 130 is interrupted" );

		// Unknown flags are collected, not ignored -- and they do NOT consume the
		// following token, because a flag we do not recognise has no arity. So in
		// this line "5" is the prompt, not the value of the typo.
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

		// An unknown flag does not swallow the next token: a second bare argument is
		// also collected, so nothing is silently dropped.
		auto arity = mcode::cli::parse_exec_options( { "--max-step", "5", "prompt" } );

		check( static_cast< bool >( arity ), "parsed with an unknown flag present" );

		if ( arity ) {
			check( arity->prompt == "5", "the unknown flag did not consume its neighbour" );
			check( arity->unknown_arguments.size( ) == 2,
				"both the typo and the extra bare argument were collected" );
		}

		// A flag missing its value is an error, not an empty string.
		auto missing = mcode::cli::parse_exec_options( { "--model" } );
		check( !missing, "a flag without a value is refused" );

		auto bad_number = mcode::cli::parse_exec_options( { "--max-steps", "lots" } );
		check( !bad_number, "a non-numeric flag value is refused" );

		// The JSON stream pairs run.start with exactly one run.end.
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
	section( "extension loader (docs/26 E8)" );

	{
		// The real path: discover, validate, install the frozen API, run init.luau,
		// and call the registered tool back. No stubs.
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

		// The fixture root deliberately holds one good extension and one whose
		// manifest has a typo. Both outcomes are the point: the good one loads, the
		// bad one fails, and the failure does not take the session with it.
		check( loaded.report.loaded.size( ) == 1, "the valid fixture extension loaded" );
		check( loaded.report.failed.size( ) == 1, "the invalid fixture extension failed" );
		check( loaded.report.loaded.front( ).name == "hello-tool",
			"the loaded extension is the valid one" );

		// A tool an extension registered is indistinguishable from a core tool to
		// the registry, except for its source and owner.
		const auto* hello = registry.find( "hello" );
		check( hello != nullptr, "the extension's tool is in the registry" );

		if ( hello != nullptr ) {
			check( hello->owner == "hello-tool", "the tool is attributed to its extension" );
			check( !hello->is_core( ), "the tool is not core" );
		}

		// And it runs: C++ registry -> VM closure -> extension code -> back.
		auto greeting = loaded.invoke( "hello", R"({"name":"smoke"})" );
		check( greeting && greeting->find( "hello from hello-tool to smoke" ) != std::string::npos,
			"the tool ran and returned the extension's answer",
			greeting ? *greeting : greeting.error( ).msg );

		// An environmental failure comes back as a message, not an empty success.
		auto refused = loaded.invoke( "hello", "{}" );
		check( !refused && refused.error( ).msg == "name is required",
			"an environmental failure returns the extension's message",
			refused ? std::string{ "unexpectedly succeeded" } : refused.error( ).msg );

		section( "extension hooks (a hook written in Luau)" );

		// The batch's exit criterion names a provider, a tool, AND a hook. The hook is
		// registered by init.luau, dispatched by the bus, and its veto is attributed.
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

		// A custom event dispatches inside the hook registry and never becomes a
		// session kind: the log's schema is closed.
		if ( !loaded.extensions.empty( ) && loaded.extensions.front( ).host != nullptr ) {
			check( static_cast< bool >( hooks.emit( "hello-tool.ready", R"({"count":7})" ) ),
				"a custom event reaches the extension" );
		}

		check( hooks.total_failures( ) == 0, "no hook threw" );

		section( "extension loader (disable all)" );

		// The mechanical check: with every extension disabled the agent must
		// still have its core tools and must not error. This is what proves the
		// core/extension line has not drifted.
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

		// Invoking what would have been there is a clean error, not a crash.
		auto absent = bare.invoke( "hello", "{}" );
		check( !absent, "invoking a disabled tool is an error" );

		section( "extension loader (shipped extensions)" );

		// The extensions the product ships. `providers` declares three model
		// providers through `mcode.model.register`, which is the D3 dogfood claim
		// exercised through the real loader rather than a hand-written mirror.
		auto shipped_registry = mcode::tool_registry{ };
		auto shipped_providers = mcode::model::provider_registry{ };
		auto shipped_bus = mcode::events::bus{ };
		auto shipped_hooks = mcode::ext::hook_registry{ shipped_bus };

		auto shipped_options = mcode::ext::loader_options{ };
		shipped_options.register_api = mcode::ext::default_register_api( shipped_registry );

		auto shipped = mcode::ext::load_extensions(
			{ std::filesystem::path{ MCODE_SMOKE_SHIPPED_EXTENSIONS } }, shipped_providers,
			shipped_hooks, shipped_options );

		for ( const auto& failure : shipped.report.failed ) {
			std::printf( "  loader: %s failed: %s\n", failure.name.c_str( ),
				failure.reason.c_str( ) );
		}

		check( shipped.report.loaded.size( ) == 2, "both shipped extensions loaded" );
		check( shipped.report.failed.empty( ), "every shipped extension loaded" );
		check( shipped_providers.size( ) == 3, "the providers extension declared three providers" );
		check( shipped_providers.find( "openai-chat-completions" ) != nullptr,
			"the OpenAI descriptor is registered" );
		check( shipped_registry.find( "skill_read" ) != nullptr,
			"the skills extension registered skill_read" );

		if ( const auto* descriptor = shipped_providers.find( "anthropic-messages" );
			descriptor != nullptr ) {
			check( descriptor->endpoint.starts_with( "https://" ),
				"the descriptor carries a real endpoint" );
		}
	}
}
