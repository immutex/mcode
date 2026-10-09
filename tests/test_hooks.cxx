#include "ext_test_helpers.hxx"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace mcode;
using namespace ext_test;

TEST_CASE( "a hook written in Luau vetoes through the bus", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { extensions_root( ) }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( hooks.size( ) == 2 );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 1 );
	REQUIRE( hooks.handlers_for( "hello-tool.ready" ) == 1 );

	auto allowed = events::event{ };
	allowed.type = events::kind::tool_pre_call;
	allowed.sequence = 1;
	allowed.payload_json = R"({"name":"read"})";

	REQUIRE_FALSE( static_cast< bool >( bus.publish( allowed ) ) );

	auto blocked = events::event{ };
	blocked.type = events::kind::tool_pre_call;
	blocked.sequence = 2;
	blocked.payload_json = R"({"name":"forbidden"})";

	auto veto = bus.publish( blocked );

	REQUIRE( static_cast< bool >( veto ) );

	if ( veto ) {
		REQUIRE( veto->reason == "blocked by hello-tool" );
		REQUIRE( veto->source == "hello-tool" );
	}

	auto notification = events::event{ };
	notification.type = events::kind::tool_result;
	notification.sequence = 3;
	notification.payload_json = R"({"name":"forbidden"})";

	REQUIRE_FALSE( static_cast< bool >( bus.publish( notification ) ) );
}

TEST_CASE( "a custom event reaches the extension and never the log", "[loader]" ) {
	// everything the api surface or a hook holds must outlive the load, not just the iteration
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { extensions_root( ) }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.size( ) == 1 );

	if ( loaded.extensions.empty( ) ) {
		return;
	}

	REQUIRE( static_cast< bool >( hooks.emit( "hello-tool.ready", R"({"count":7})" ) ) );
	REQUIRE( hooks.total_failures( ) == 0 );
	REQUIRE( hooks.failures( "hello-tool" ) == 0 );

	REQUIRE( static_cast< bool >( hooks.emit( "hello-tool.nothing", "{}" ) ) );

	auto refused = hooks.emit( "tool.call", "{}" );
	REQUIRE_FALSE( static_cast< bool >( refused ) );
	REQUIRE( refused.error( ).msg.find( "session event" ) != std::string::npos );
}

TEST_CASE( "a throwing hook is contained, counted, and does not abort dispatch", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "throwing-hook";

	write( directory / "ext.toml",
		"name = \"throwing-hook\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(mcode.on("tool.pre_call", function(ev)
	error("hook exploded")
end)

mcode.on("tool.pre_call", function(ev)
	if ev.payload.name == "second" then
		return { veto = "second handler ran" }
	end
end)
)LUASRC" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 2 );

	auto event = events::event{ };
	event.type = events::kind::tool_pre_call;
	event.payload_json = R"({"name":"second"})";

	auto veto = bus.publish( event );

	REQUIRE( static_cast< bool >( veto ) );

	if ( veto ) {
		REQUIRE( veto->reason == "second handler ran" );
	}

	REQUIRE( hooks.total_failures( ) >= 1 );
	REQUIRE( hooks.failures( "throwing-hook" ) >= 1 );

	std::filesystem::remove_all( root );
}

TEST_CASE( "a clean call clears the consecutive-failure counter", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { extensions_root( ) }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.size( ) == 1 );

	auto event = events::event{ };
	event.type = events::kind::tool_pre_call;
	event.payload_json = R"({"name":"read"})";

	bus.publish( event );

	REQUIRE( hooks.failures( "hello-tool" ) == 0 );
}

TEST_CASE( "an unrecognised hook name is refused, not silently inert", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "typo-hook";

	write( directory / "ext.toml",
		"name = \"typo-hook\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau",
		R"LUASRC(mcode.on("tool.precal", function(ev) return nil end))LUASRC" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.failed.size( ) == 1 );
	REQUIRE( loaded.report.failed.front( ).reason.find( "tool.precal" ) != std::string::npos );

	std::filesystem::remove_all( root );
}

TEST_CASE( "unloading an extension detaches its hooks", "[loader]" ) {
	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { extensions_root( ) }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( hooks.size( ) == 2 );

	if ( loaded.extensions.empty( ) ) {
		return;
	}

	REQUIRE( hooks.detach_owner( *loaded.extensions.front( ).host, "hello-tool" ) == 2 );
	REQUIRE( hooks.size( ) == 0 );

	auto event = events::event{ };
	event.type = events::kind::tool_pre_call;
	event.payload_json = R"({"name":"forbidden"})";

	REQUIRE_FALSE( static_cast< bool >( bus.publish( event ) ) );
}

TEST_CASE( "a failing extension leaves nothing behind in the shared registries", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "partial-ext";

	write( directory / "ext.toml",
		"name = \"partial-ext\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(mcode.on("tool.pre_call", function(ev)
	return { veto = "dangling" }
end)

mcode.tool.register({
	name = "ghost",
	description = "registered, then the extension dies",
	schema = { type = "object" },
	run = function(args, ctx) return "never" end,
})

error("deliberate failure after registering")
)LUASRC" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.failed.size( ) == 1 );
	REQUIRE( loaded.report.loaded.empty( ) );

	CHECK( registry.find( "ghost" ) == nullptr );
	CHECK( registry.owned_by( "partial-ext" ).empty( ) );

	CHECK( hooks.handlers_for( "tool.pre_call" ) == 0 );
	CHECK( hooks.size( ) == 0 );

	auto event = events::event{ };
	event.type = events::kind::tool_pre_call;
	event.payload_json = R"({"name":"anything"})";

	CHECK_FALSE( static_cast< bool >( bus.publish( event ) ) );

	std::filesystem::remove_all( root );
}

TEST_CASE( "require loads a module from inside the extension", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "modular";

	write( directory / "ext.toml",
		"name = \"modular\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "lib" / "greeting.luau",
		"return { word = \"hello from a module\" }\n" );
	write( directory / "init.luau", R"LUASRC(local greeting = require("./lib/greeting")

mcode.tool.register({
	name = "greet",
	description = "Return the module's word",
	schema = { type = "object", properties = {} },
	run = function(args, ctx)
		return greeting.word, nil
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

	INFO( describe( loaded.report ) );
	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( registry.find( "greet" ) != nullptr );

	auto called = loaded.invoke( "greet", "{}" );
	REQUIRE( static_cast< bool >( called ) );

	if ( called ) {
		REQUIRE( *called == "hello from a module" );
	}

	std::filesystem::remove_all( root );
}

TEST_CASE( "require cannot escape the extension root", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "escaping";

	write( root / "outside.luau", "return { secret = \"outside the root\" }\n" );
	write( directory / "ext.toml",
		"name = \"escaping\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(local escaped = require("../outside")

mcode.log.info("escaped", escaped.secret)
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

	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.failed.size( ) == 1 );
	REQUIRE( loaded.report.failed.front( ).reason.find( "module not found" ) != std::string::npos );

	std::filesystem::remove_all( root );
}

TEST_CASE( "five consecutive handler failures quarantine the extension", "[loader]" ) {
	// the count is consecutive, not cumulative: any clean call resets the streak
	const auto root = scratch_root( );
	const auto directory = root / "always-throws";

	write( directory / "ext.toml",
		"name = \"always-throws\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(mcode.on("tool.pre_call", function(ev)
	error("this handler always fails")
end)
)LUASRC" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 1 );
	REQUIRE_FALSE( hooks.quarantined( "always-throws" ) );

	auto event = events::event{ };
	event.type = events::kind::tool_pre_call;
	event.payload_json = R"({"name":"read"})";

	for ( auto attempt = 0; attempt < 4; ++attempt ) {
		bus.publish( event );
	}

	REQUIRE( hooks.failures( "always-throws" ) == 4 );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 1 );
	REQUIRE_FALSE( hooks.quarantined( "always-throws" ) );

	bus.publish( event );

	REQUIRE( hooks.quarantined( "always-throws" ) );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 0 );
	REQUIRE( hooks.size( ) == 0 );

	auto refused = hooks.subscribe( *loaded.extensions.front( ).host, "tool.pre_call", 0,
		"always-throws" );

	REQUIRE_FALSE( static_cast< bool >( refused ) );
	REQUIRE( refused.error( ).msg.find( "quarantined" ) != std::string::npos );

	std::filesystem::remove_all( root );
}

TEST_CASE( "a clean call clears the consecutive-failure streak", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "flaps";

	write( directory / "ext.toml",
		"name = \"flaps\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(mcode.on("tool.pre_call", function(ev)
	if ev.payload.name == "explode" then
		error("deliberate")
	end

	return nil
end)
)LUASRC" );

	auto registry = tool_registry{ };
	g_registry = &registry;

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { root }, providers, hooks, options );

	REQUIRE( loaded.report.loaded.size( ) == 1 );

	auto exploding = events::event{ };
	exploding.type = events::kind::tool_pre_call;
	exploding.payload_json = R"({"name":"explode"})";

	auto clean = events::event{ };
	clean.type = events::kind::tool_pre_call;
	clean.payload_json = R"({"name":"read"})";

	for ( auto round = 0; round < 4; ++round ) {
		for ( auto attempt = 0; attempt < 4; ++attempt ) {
			bus.publish( exploding );
		}

		bus.publish( clean );

		REQUIRE( hooks.failures( "flaps" ) == 0 );
	}

	REQUIRE_FALSE( hooks.quarantined( "flaps" ) );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 1 );
	REQUIRE( hooks.total_failures( ) >= 16 );

	std::filesystem::remove_all( root );
}

TEST_CASE( "the deepseek guard reports leaked tool-call markup and stays quiet otherwise",
	"[loader][deepseek-guard]" ) {
	// Loads the SHIPPED extension, not a fixture: the guard's value is that it is
	// wired to a real event, and a fixture copy would keep passing after the
	// shipped one stopped loading.
	auto registry = tool_registry{ };
	g_registry = &registry;
	g_notifications.clear( );

	auto providers = model::provider_registry{ };
	auto bus = events::bus{ };
	auto hooks = ext::hook_registry{ bus };

	auto options = ext::loader_options{ };
	options.register_api = register_api;

	auto loaded = ext::load_extensions( { std::filesystem::path{ MCODE_EXTENSIONS_ROOT } },
		providers, hooks, options );

	REQUIRE( loaded.report.failed.empty( ) );

	auto guard_loaded = false;

	for ( const auto& entry : loaded.report.loaded ) {
		if ( entry.name == "deepseek-guard" ) {
			guard_loaded = true;
		}
	}

	REQUIRE( guard_loaded );
	REQUIRE( hooks.handlers_for( "assistant.delta" ) >= 1 );

	auto turn = events::event{ };
	turn.type = events::kind::turn_start;
	turn.payload_json = R"({})";
	bus.publish( turn );

	// Ordinary prose must not fire it, or the guard is noise.
	auto prose = events::event{ };
	prose.type = events::kind::assistant_delta;
	prose.payload_json = R"({"text":"The header markup is identical on all pages."})";
	bus.publish( prose );

	REQUIRE( g_notifications.empty( ) );

	// The ASCII wrapper form.
	auto leak = events::event{ };
	leak.type = events::kind::assistant_delta;
	leak.payload_json = R"({"text":"Sure. <|DSML|tool_calls> {\"name\":\"write\"}"})";
	bus.publish( leak );

	REQUIRE( g_notifications.size( ) == 1 );
	REQUIRE( g_notifications.front( ).find( "deepseek-guard" ) != std::string::npos );
	REQUIRE( g_notifications.front( ).find( "warn" ) != std::string::npos );

	// Once per turn: a leaked wrapper arrives across many deltas and a
	// notification per delta would be the failure rather than the report.
	bus.publish( leak );
	REQUIRE( g_notifications.size( ) == 1 );

	// The next turn reports again, because a second leak is a second event.
	bus.publish( turn );
	bus.publish( leak );
	REQUIRE( g_notifications.size( ) == 2 );

	// The full-width `｜` (U+FF5C) form is the one the DeepSeek serving path
	// actually emits, and the extension writes it as a Lua `\u{FF5C}` escape.
	// Built here from raw UTF-8 bytes (EF BD 9C) rather than a JSON `\u` escape,
	// because the payload decoder does not interpret those. If the Lua escape did
	// not produce this character the marker would never match, and this is the
	// only thing that would say so.
	auto full_width = events::event{ };
	full_width.type = events::kind::assistant_delta;
	full_width.payload_json = std::string{ "{\"text\":\"Sure. " } + "\xEF\xBD\x9C" +
		"DSML" + "\xEF\xBD\x9C" + "tool_calls>\"}";

	bus.publish( turn );
	bus.publish( full_width );

	REQUIRE( g_notifications.size( ) == 3 );
}
