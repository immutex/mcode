#include "ext_test_helpers.hxx"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "mcode/support/json.hxx"

using namespace mcode;
using namespace ext_test;

namespace {

	// A bundled extension is a directory holding an `ext.toml`. Counting them
	// rather than pinning a literal means adding an extension does not require
	// editing these tests -- and a bundled extension that fails to load is still
	// caught, because the loaded count would come up short of it. Takes the roots
	// the caller actually loads, since a test may load more than one.
	[[nodiscard]] auto extension_count_in( const std::vector< std::filesystem::path >& roots )
		-> std::size_t {
		auto count = std::size_t{ 0 };

		for ( const auto& root : roots ) {
			if ( !std::filesystem::is_directory( root ) ) {
				continue;
			}

			for ( const auto& entry : std::filesystem::directory_iterator{ root } ) {
				if ( std::filesystem::exists( entry.path( ) / "ext.toml" ) ) {
					++count;
				}
			}
		}

		return count;
	}

}

TEST_CASE( "loading registers nothing when extensions are disabled", "[loader]" ) {
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

	REQUIRE( report.failed.size( ) == 1 );
	REQUIRE( report.failed.front( ).name == "broken-manifest" );
}

TEST_CASE( "a bad manifest fails the extension, not the session", "[loader]" ) {
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

	REQUIRE( registry.find( "read" ) != nullptr );
	REQUIRE( registry.find( "other" ) != nullptr );

	REQUIRE( ext::unload_extension( registry, "never-loaded" ) == 0 );
	REQUIRE( registry.size( ) == 2 );
}

TEST_CASE( "the shipped reference providers load", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;
	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto report = ext::load_extensions( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } }, providers, hooks,
		options );

	if ( report.report.loaded.size( ) != extension_count_in( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } } ) ) {
		FAIL( "unexpected report:" << describe( report.report ) );
	}

	REQUIRE( report.report.loaded.size( ) ==
		extension_count_in( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } } ) );

	const auto providers_entry = std::find_if( report.report.loaded.begin( ),
		report.report.loaded.end( ), []( const ext::load_outcome& outcome ) {
			return outcome.name == "providers";
		} );

	REQUIRE( providers_entry != report.report.loaded.end( ) );
	REQUIRE( providers_entry->bytes_used > 0 );
}

TEST_CASE( "a registered tool is callable and reaches the extension", "[loader]" ) {
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

	// non-core tool sources must carry an owner
	const auto* definition = registry.find( "hello" );

	REQUIRE( definition != nullptr );

	if ( definition == nullptr ) {
		return;
	}

	REQUIRE( definition->owner == "hello-tool" );
	REQUIRE_FALSE( definition->is_core( ) );

	REQUIRE_FALSE( definition->schema_json.empty( ) );

	if ( auto parsed = json::document::parse( definition->schema_json ); !parsed ) {
		FAIL( "the registered schema is not JSON: " << parsed.error( ).msg );
	}

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
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } }, providers, hooks,
		options );

	if ( loaded.report.loaded.size( ) != extension_count_in( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } } ) ) {
		FAIL( "unexpected report:" << describe( loaded.report ) );
	}

	REQUIRE( providers.size( ) == 3 );
	REQUIRE( providers.find( "openai-chat-completions" ) != nullptr );
	REQUIRE( providers.find( "anthropic-messages" ) != nullptr );

	for ( const auto* descriptor : providers.all( ) ) {
		REQUIRE_FALSE( descriptor->endpoint.empty( ) );
		REQUIRE( descriptor->endpoint.starts_with( "https://" ) );
	}
}

TEST_CASE( "disabling every extension leaves a working registry", "[loader]" ) {
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

	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.failed.empty( ) );
	REQUIRE( loaded.extensions.empty( ) );
	REQUIRE( loaded.report.disabled ==
		extension_count_in( { extensions_root( ), std::filesystem::path{ MCODE_EXTENSIONS_ROOT } } ) );

	REQUIRE( registry.size( ) == core_count );
	REQUIRE( registry.find( "hello" ) == nullptr );
	REQUIRE( registry.find( "read" ) != nullptr );

	REQUIRE_FALSE( static_cast< bool >( loaded.invoke( "hello", "{}" ) ) );
	REQUIRE_FALSE( static_cast< bool >( loaded.invoke( "web_search", "{}" ) ) );

	REQUIRE( providers.empty( ) );
}

TEST_CASE( "the surface exposes exactly the frozen fields", "[loader]" ) {
	// mcode.api_version must be a number: a number/string comparison raises instead of coercing
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

	auto name = host.eval_to_string( "mcode.ext.name" );
	REQUIRE( static_cast< bool >( name ) );
	REQUIRE( *name == "hello-tool" );

	auto version_is_number = host.eval_to_string( "type(mcode.api_version) == 'number'" );
	REQUIRE( static_cast< bool >( version_is_number ) );
	REQUIRE( *version_is_number == "true" );

	auto version = host.eval_to_string( "mcode.api_version" );
	REQUIRE( static_cast< bool >( version ) );
	REQUIRE( *version == std::to_string( ext::API_VERSION ) );

	auto compares = host.eval_to_string( "mcode.api_version >= 1" );
	REQUIRE( static_cast< bool >( compares ) );
	REQUIRE( *compares == "true" );

	auto shape = host.eval_to_string(
		"type(mcode.tool) == 'table' and type(mcode.tool.register) == 'function'" );
	REQUIRE( static_cast< bool >( shape ) );
	REQUIRE( *shape == "true" );
}

TEST_CASE( "a required module's compile error reaches the host", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "broken-module";

	write( directory / "ext.toml",
		"name = \"broken-module\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", "local value = require(\"helper\")\nreturn value\n" );
	write( directory / "helper.luau", "local x = = 1\n" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };
	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.failed.size( ) == 1 );

	if ( loaded.report.failed.size( ) == 1 ) {
		// the compiler's line number, not the bare module path the raise would otherwise carry
		REQUIRE( loaded.report.failed.front( ).reason.find( ":1:" ) != std::string::npos );
	}

	std::filesystem::remove_all( root );
}

TEST_CASE( "a schema table with mixed key types keeps every entry", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "mixed-schema";

	write( directory / "ext.toml",
		"name = \"mixed-schema\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(mcode.tool.register({
	name = "mixed",
	description = "A schema with array entries beside named keys",
	schema = { "first", "second", type = "object", note = "kept" },
	run = function(args, ctx)
		return "ok", nil
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

	const auto* definition = registry.find( "mixed" );

	REQUIRE( definition != nullptr );

	if ( definition == nullptr ) {
		return;
	}

	auto parsed = json::document::parse( definition->schema_json );

	REQUIRE( static_cast< bool >( parsed ) );

	if ( !parsed ) {
		FAIL( "the registered schema is not JSON: " << parsed.error( ).msg );
	}

	// a dropped entry silently changes the schema the model is shown
	auto first = parsed->pointer( "/1" );
	REQUIRE( static_cast< bool >( first ) );

	if ( first ) {
		REQUIRE( *first == "first" );
	}

	auto second = parsed->pointer( "/2" );
	REQUIRE( static_cast< bool >( second ) );

	if ( second ) {
		REQUIRE( *second == "second" );
	}

	auto note = parsed->pointer( "/note" );
	REQUIRE( static_cast< bool >( note ) );

	if ( note ) {
		REQUIRE( *note == "kept" );
	}

	std::filesystem::remove_all( root );
}

TEST_CASE( "an unsigned argument past the signed range survives the JSON crossing", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "big-integer";

	write( directory / "ext.toml",
		"name = \"big-integer\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(mcode.tool.register({
	name = "magnitude",
	description = "Reports the sign of the argument",
	schema = { type = "object" },
	run = function(args, ctx)
		if args.value > 0 then
			return "positive", nil
		end

		return "not positive", nil
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

	// 2^64-1 is stored as an unsigned payload; reading it as signed would make it -1
	auto result = loaded.invoke( "magnitude", R"({"value":18446744073709551615})" );

	REQUIRE( static_cast< bool >( result ) );

	if ( !result ) {
		FAIL( "invoking the tool failed: " << result.error( ).msg );
	}

	REQUIRE( *result == "positive" );

	std::filesystem::remove_all( root );
}

