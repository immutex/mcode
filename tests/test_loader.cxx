#include "ext_test_helpers.hxx"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "mcode/support/json.hxx"

using namespace mcode;
using namespace ext_test;

TEST_CASE( "loading registers nothing when extensions are disabled", "[loader]" ) {
	// E8's acceptance: disabling every extension leaves a working, less capable
	// agent. The registry must be untouched, and the report must say the
	// extensions were DISABLED rather than missing.
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.disabled = true;
	options.register_api = register_api;

	auto report = ext::load_extensions( { extensions_root( ) }, providers, hooks, options ).report;

	REQUIRE( report.loaded.empty( ) );
	REQUIRE( report.total_tools( ) == 0 );
	REQUIRE( registry.empty( ) );

	// Two extension directories exist, and both are accounted for as disabled.
	REQUIRE( report.disabled == 2 );
	REQUIRE( report.failed.empty( ) );
}

TEST_CASE( "a disabled-name list skips without failing", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.disabled_names = { "hello-tool" };
	options.register_api = register_api;

	auto report = ext::load_extensions( { extensions_root( ) }, providers, hooks, options ).report;

	REQUIRE( report.disabled == 1 );

	// broken-manifest still fails on its own merits; disabling is unrelated.
	REQUIRE( report.failed.size( ) == 1 );
	REQUIRE( report.failed.front( ).name == "broken-manifest" );
}

TEST_CASE( "a bad manifest fails the extension, not the session", "[loader]" ) {
	// A bad manifest fails the extension, never the session. The valid
	// extension alongside it must still load.
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto report = ext::load_extensions( { extensions_root( ) }, providers, hooks, options ).report;

	if ( report.loaded.size( ) != 1 ) {
		auto reasons = std::string{ };

		for ( const auto& failure : report.failed ) {
			reasons += "\n  failed " + failure.name + ": " + failure.reason;
		}

		for ( const auto& entry : report.loaded ) {
			reasons += "\n  loaded " + entry.name;
		}

		FAIL( "expected exactly one loaded extension; got " << report.loaded.size( ) << reasons );
	}

	REQUIRE( report.loaded.size( ) == 1 );
	REQUIRE( report.loaded.front( ).name == "hello-tool" );

	REQUIRE( report.failed.size( ) == 1 );
	REQUIRE( report.failed.front( ).name == "broken-manifest" );
	REQUIRE_FALSE( report.failed.front( ).reason.empty( ) );
}

TEST_CASE( "load order is deterministic", "[loader]" ) {
	// Two runs must load in the same sequence, or a duplicate-name report would
	// differ run to run.
	for ( auto attempt = 0; attempt < 3; ++attempt ) {
		auto registry = tool_registry{ };
		g_registry = &registry;
		auto providers = model::provider_registry{ };
		auto bus = events::bus{ };
		auto hooks = ext::hook_registry{ bus };
		auto options = ext::loader_options{ };
		options.register_api = register_api;

		auto report = ext::load_extensions( { extensions_root( ) }, providers, hooks, options ).report;

		if ( report.loaded.size( ) != 1 || report.failed.size( ) != 1 ) {
			FAIL( "unexpected report:" << describe( report ) );
		}

		REQUIRE( report.loaded.size( ) == 1 );
		REQUIRE( report.loaded.front( ).name == "hello-tool" );
		REQUIRE( report.failed.size( ) == 1 );
		REQUIRE( report.failed.front( ).name == "broken-manifest" );
	}
}

TEST_CASE( "a missing root is skipped, not an error", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto report = ext::load_extensions(
		{ std::filesystem::temp_directory_path( ) / "mcode-no-such-root" }, providers, hooks,
		options ).report;

	REQUIRE( report.loaded.empty( ) );
	REQUIRE( report.failed.empty( ) );
	REQUIRE( report.disabled == 0 );
}

TEST_CASE( "unloading removes exactly the owner's tools", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto core = tool_def{ };
	core.name = "read";
	core.source = tool_source::core;
	REQUIRE( static_cast< bool >( registry.add( std::move( core ) ) ) );

	auto extension_tool = tool_def{ };
	extension_tool.name = "hello";
	extension_tool.source = tool_source::user_extension;
	extension_tool.owner = "hello-tool";
	REQUIRE( static_cast< bool >( registry.add( std::move( extension_tool ) ) ) );

	auto other = tool_def{ };
	other.name = "other";
	other.source = tool_source::user_extension;
	other.owner = "other-ext";
	REQUIRE( static_cast< bool >( registry.add( std::move( other ) ) ) );

	REQUIRE( registry.size( ) == 3 );

	const auto removed = ext::unload_extension( registry, "hello-tool" );

	REQUIRE( removed == 1 );
	REQUIRE( registry.size( ) == 2 );
	REQUIRE( registry.find( "hello" ) == nullptr );

	// Core tools and other extensions are untouched.
	REQUIRE( registry.find( "read" ) != nullptr );
	REQUIRE( registry.find( "other" ) != nullptr );

	// Unloading something that is not loaded is a no-op, not an error.
	REQUIRE( ext::unload_extension( registry, "never-loaded" ) == 0 );
	REQUIRE( registry.size( ) == 2 );
}

TEST_CASE( "the shipped reference providers load", "[loader]" ) {
	// extensions/providers is a real first-party extension; if it cannot load, the
	// dogfood claim in D3 is empty.
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto report = ext::load_extensions( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } }, providers, hooks,
		options ).report;

	// It registers providers rather than tools, so the registry stays empty --
	// but it must not be reported as failed.
	if ( report.loaded.size( ) != 1 ) {
		FAIL( "unexpected report:" << describe( report ) );
	}

	REQUIRE( report.loaded.size( ) == 1 );
	REQUIRE( report.loaded.front( ).name == "providers" );
	REQUIRE( report.loaded.front( ).bytes_used > 0 );
}

TEST_CASE( "a registered tool is callable and reaches the extension", "[loader]" ) {
	// The dogfood assertion for E8: not "the file parsed" but "the model can call
	// this tool and get the extension's answer". A registry entry with no live VM
	// behind it would pass every structural check and fail this one.
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { extensions_root( ) }, providers, hooks, options );

	if ( loaded.report.loaded.size( ) != 1 ) {
		FAIL( "unexpected report:" << describe( loaded.report ) );
	}

	// The tool is in the registry, attributed to the extension rather than to
	// core, and non-core sources must carry an owner.
	const auto* definition = registry.find( "hello" );

	REQUIRE( definition != nullptr );

	if ( definition == nullptr ) {
		return;
	}

	REQUIRE( definition->owner == "hello-tool" );
	REQUIRE_FALSE( definition->is_core( ) );

	// The schema has to survive registration: the loop builds the request's tool
	// array from the registry, so a tool whose schema is dropped here is callable
	// but never advertised to the model.
	REQUIRE_FALSE( definition->schema_json.empty( ) );

	if ( auto parsed = json::document::parse( definition->schema_json ); !parsed ) {
		FAIL( "the registered schema is not JSON: " << parsed.error( ).msg );
	}

	// And it runs. The extension reads README.md through mcode.fs.read, which is
	// the fixture's own file, so the result proves the round trip: C++ registry ->
	// VM closure -> extension code -> back to C++.
	auto result = loaded.invoke( "hello", R"({"name":"world"})" );

	REQUIRE( static_cast< bool >( result ) );

	if ( !result ) {
		FAIL( "invoking the tool failed: " << result.error( ).msg );
	}

	REQUIRE( result->find( "hello from hello-tool" ) != std::string::npos );
}

TEST_CASE( "invoking an unknown tool is an error, not a crash", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { extensions_root( ) }, providers, hooks, options );

	auto missing = loaded.invoke( "no-such-tool", "{}" );
	REQUIRE_FALSE( static_cast< bool >( missing ) );
	REQUIRE( missing.error( ).msg.find( "no-such-tool" ) != std::string::npos );
}

TEST_CASE( "an environmental failure returns the extension's message", "[loader]" ) {
	// Failure splits in two: `nil, err` is environmental and returns to
	// the caller, a raised error is a contract violation. Both must reach the
	// model as a message rather than as an empty success.
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { extensions_root( ) }, providers, hooks, options );

	auto missing_name = loaded.invoke( "hello", "{}" );

	REQUIRE_FALSE( static_cast< bool >( missing_name ) );

	if ( missing_name ) {
		return;
	}

	REQUIRE( missing_name.error( ).msg == "name is required" );
}

TEST_CASE( "a raised error does not become an empty result", "[loader]" ) {
	// A contract violation raises rather than returning. If the host treated the
	// missing return value as success, the model would see an empty tool result
	// and have no way to tell it apart from a tool that legitimately returned "".
	const auto root = scratch_root( );
	const auto directory = root / "raising-tool";

	write( directory / "ext.toml",
		"name = \"raising-tool\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(mcode.tool.register({
	name = "boom",
	description = "Always raises",
	schema = { type = "object" },
	run = function(args, ctx)
		error("kaboom")
	end,
})
)LUASRC" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	if ( loaded.report.loaded.size( ) != 1 ) {
		FAIL( "unexpected report:" << describe( loaded.report ) );
	}

	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( registry.find( "boom" ) != nullptr );

	auto result = loaded.invoke( "boom", "{}" );

	REQUIRE_FALSE( static_cast< bool >( result ) );

	if ( result ) {
		return;
	}

	REQUIRE( result.error( ).msg.find( "kaboom" ) != std::string::npos );

	std::filesystem::remove_all( root );
}

TEST_CASE( "the providers extension declares three providers through the API", "[loader]" ) {
	// D3's claim is that a real provider ships as an extension. This asserts it
	// through the loader rather than through a hand-written JSON mirror: the
	// .luau file is executed and its `mcode.model.register` calls land in the
	// provider registry.
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } }, providers, hooks,
		options );

	if ( loaded.report.loaded.size( ) != 1 ) {
		FAIL( "unexpected report:" << describe( loaded.report ) );
	}

	REQUIRE( providers.size( ) == 3 );
	REQUIRE( providers.find( "openai-chat-completions" ) != nullptr );
	REQUIRE( providers.find( "anthropic-messages" ) != nullptr );

	// Descriptors are data with no owner recorded, so unloading the extension
	// must not silently drop them: the registry is host state.
	for ( const auto* descriptor : providers.all( ) ) {
		REQUIRE_FALSE( descriptor->endpoint.empty( ) );
		REQUIRE( descriptor->endpoint.starts_with( "https://" ) );
	}
}

TEST_CASE( "disabling every extension leaves a working registry", "[loader]" ) {
	// The acceptance criterion, stated as "the mechanical check that
	// the core/extension line has not drifted": with every extension disabled the
	// agent must still have its core tools and must not error.
	auto registry = tool_registry{ };

	for ( const auto* name : { "read", "write", "edit", "glob", "grep", "bash" } ) {
		auto core = tool_def{ };
		core.name = name;
		core.source = tool_source::core;
		REQUIRE( static_cast< bool >( registry.add( std::move( core ) ) ) );
	}

	const auto core_count = registry.size( );

	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.disabled = true;
	options.register_api = register_api;

	auto loaded = ext::load_extensions(
		{ extensions_root( ), std::filesystem::path{ MCODE_EXTENSIONS_ROOT } }, providers,
		hooks, options );

	// Nothing loaded, nothing failed, everything accounted for as disabled.
	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.failed.empty( ) );
	REQUIRE( loaded.extensions.empty( ) );
	REQUIRE( loaded.report.disabled == 3 );

	// The core tools are intact, and no extension tool appeared.
	REQUIRE( registry.size( ) == core_count );
	REQUIRE( registry.find( "hello" ) == nullptr );
	REQUIRE( registry.find( "read" ) != nullptr );

	// And invoking anything an extension would have provided is a clean error
	// rather than a crash or a hang.
	REQUIRE_FALSE( static_cast< bool >( loaded.invoke( "hello", "{}" ) ) );
	REQUIRE_FALSE( static_cast< bool >( loaded.invoke( "web_search", "{}" ) ) );

	// Nothing reached the provider registry either.
	REQUIRE( providers.empty( ) );
}

TEST_CASE( "the surface exposes exactly the frozen fields", "[loader]" ) {
	// The frozen surface has `mcode.ext.name` and `mcode.api_version`, and closes the
	// table. A field that is not in it is drift, and `api_version` as a string
	// would break every `mcode.api_version < 2` comparison an author writes --
	// Luau raises on a number/string comparison rather than coercing.
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { extensions_root( ) }, providers, hooks, options );

	REQUIRE( loaded.extensions.size( ) == 1 );

	if ( loaded.extensions.empty( ) ) {
		return;
	}

	auto& host = *loaded.extensions.front( ).host;

	// The extension ran, so the surface is sealed: every probe is a read.
	auto name = host.eval_to_string( "mcode.ext.name" );
	REQUIRE( static_cast< bool >( name ) );
	REQUIRE( *name == "hello-tool" );

	// A number, not a string. This is the assertion that catches the coercion bug
	// the type checker cannot see, because the definition file declares the type
	// rather than reading it.
	auto version_is_number = host.eval_to_string( "type(mcode.api_version) == 'number'" );
	REQUIRE( static_cast< bool >( version_is_number ) );
	REQUIRE( *version_is_number == "true" );

	auto version = host.eval_to_string( "mcode.api_version" );
	REQUIRE( static_cast< bool >( version ) );
	REQUIRE( *version == std::to_string( ext::API_VERSION ) );

	// A comparison an author actually writes must work rather than raise.
	auto compares = host.eval_to_string( "mcode.api_version >= 1" );
	REQUIRE( static_cast< bool >( compares ) );
	REQUIRE( *compares == "true" );

	// And the documented shape: `mcode.tool.register` is reachable at that exact
	// path, which is what makes the two-level namespacing real rather than
	// aspirational.
	auto shape = host.eval_to_string(
		"type(mcode.tool) == 'table' and type(mcode.tool.register) == 'function'" );
	REQUIRE( static_cast< bool >( shape ) );
	REQUIRE( *shape == "true" );
}

