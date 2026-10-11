#include "ext_test_helpers.hxx"

#include <filesystem>
#include <string>

#include "mcode/support/json.hxx"

using namespace mcode;
using namespace ext_test;

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
