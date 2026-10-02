#include "ext_test_helpers.hxx"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace mcode;
using namespace ext_test;

TEST_CASE( "an extension without init.luau is refused by name", "[loader]" ) {
	const auto root = scratch_root( );

	write( root / "no-entry" / "ext.toml",
		"name = \"no-entry\"\nversion = \"0.1.0\"\napi_version = 1\n" );

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
	REQUIRE( loaded.report.failed.front( ).name == "no-entry" );
	REQUIRE( loaded.report.failed.front( ).reason == "no init.luau" );

	std::filesystem::remove_all( root );
}

TEST_CASE( "two extensions claiming one tool name is a failure, not a silent override", "[loader]" ) {
	const auto root = scratch_root( );

	for ( const auto* name : { "first", "second" } ) {
		write( root / name / "ext.toml",
			std::string{ "name = \"" } + name + "\"\nversion = \"0.1.0\"\napi_version = 1\n" );
		write( root / name / "init.luau", R"LUASRC(mcode.tool.register({
	name = "shared",
	description = "Claimed by more than one extension",
	schema = { type = "object", properties = {} },
	run = function(args, ctx)
		return "ok", nil
	end,
})
)LUASRC" );
	}

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( loaded.report.failed.size( ) == 1 );
	REQUIRE( loaded.report.failed.front( ).reason.find( "shared" ) != std::string::npos );

	// deterministic order means the first candidate keeps a colliding name
	REQUIRE( loaded.report.loaded.front( ).name == "first" );
	REQUIRE( loaded.report.failed.front( ).name == "second" );

	std::filesystem::remove_all( root );
}

TEST_CASE( "unregister and off remove exactly what they named", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "handles";

	write( directory / "ext.toml",
		"name = \"handles\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(local kept = mcode.tool.register({
	name = "kept",
	description = "Stays registered",
	schema = { type = "object", properties = {} },
	run = function(args, ctx)
		return "kept", nil
	end,
})

local doomed = mcode.tool.register({
	name = "doomed",
	description = "Unregistered below",
	schema = { type = "object", properties = {} },
	run = function(args, ctx)
		return "doomed", nil
	end,
})

mcode.tool.unregister(doomed)

local hook = mcode.on("tool.pre_call", function(ev)
	return nil
end)

mcode.off(hook)
)LUASRC" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	INFO( describe( loaded.report ) );
	REQUIRE( loaded.report.loaded.size( ) == 1 );

	CHECK( registry.find( "doomed" ) == nullptr );
	CHECK( registry.find( "kept" ) != nullptr );

	CHECK_FALSE( static_cast< bool >( loaded.invoke( "doomed", "{}" ) ) );

	auto kept = loaded.invoke( "kept", "{}" );
	REQUIRE( static_cast< bool >( kept ) );

	if ( kept ) {
		REQUIRE( *kept == "kept" );
	}

	CHECK( hooks.size( ) == 0 );
	CHECK( hooks.handlers_for( "tool.pre_call" ) == 0 );

	std::filesystem::remove_all( root );
}

TEST_CASE( "the loader forwards the VM limits it was given", "[loader]" ) {
	// memory_limit_bytes and time_limit are the sandbox's only enforcement knobs
	const auto root = scratch_root( );
	const auto directory = root / "limited";

	write( directory / "ext.toml",
		"name = \"limited\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", "return 0\n" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;
	options.memory_limit_bytes = 4096;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	INFO( describe( loaded.report ) );

	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.failed.size( ) == 1 );

	auto generous = ext::loader_options{ };
	generous.register_api = register_api;
	generous.memory_limit_bytes = 64 * 1024 * 1024;

	auto accepted = ext::load_extensions( { root }, providers, hooks, generous );

	INFO( describe( accepted.report ) );
	REQUIRE( accepted.report.loaded.size( ) == 1 );

	std::filesystem::remove_all( root );
}
