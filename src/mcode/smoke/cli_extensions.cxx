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
#include "mcode/platform/seams.hxx"

#include "mcode/smoke/check.hxx"

using smoke::check;
using smoke::g_failures;
using smoke::section;

namespace {

	// Where the extensions that travel with the binary live.
	//
	// Resolved from the running executable, NOT from a compiled-in path. The
	// `MCODE_SMOKE_*` constants name the build machine's source tree, which exists
	// for a local build and for nobody else -- so a released binary walked into a
	// missing directory, and `directory_iterator` throws on one. That is what
	// killed the process part-way through its own smoke test.
	//
	// This is the same location the product itself uses
	// (`cli/commands.cxx`, `cli/session.cxx`), so a smoke run asserts on the
	// extensions the binary will actually load.
	[[nodiscard]] auto bundled_extensions_directory( ) -> std::filesystem::path {
		const auto executable = mcode::platform::executable_directory( );

		return executable ? *executable / "extensions" : std::filesystem::path{ };
	}

	// A shipped extension is a directory holding an `ext.toml`. Counting them
	// rather than pinning a literal means adding one does not need this edited --
	// and a shipped extension that is silently skipped, neither loaded nor
	// failed, is still caught, which a bare `failed.empty()` would not see.
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

		// A single-dash typo is as invisible as a double-dash one: it must be collected,
		// not sent to the model as the prompt.
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
	// The fixture extensions live in the source tree, so they exist only for a build
	// running from it. A released binary has no fixture tree, and asserting on files
	// it cannot reach would fail for a reason that says nothing about the binary.
	// The shipped extensions below ARE checked either way: they travel with it.
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

		// Guarded, not assumed. `MCODE_SMOKE_EXTENSIONS` is an absolute path into the
		// build machine's source tree, so it is absent for anyone running a released
		// binary -- and an unguarded `front()` on the empty result was an access
		// violation, which killed the process and truncated every check after it.
		// That is what made the shipped 0.0.2 archive fail its own smoke test.
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

	// The providers extension is not decoration: without it the binary has no
	// provider at all and cannot run a single turn. A released archive shipped
	// without this directory, and the only symptom was "no provider named
	// 'openai-chat-completions' is registered" on the user's first command.
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
