#include "ext_test_helpers.hxx"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace mcode;
using namespace ext_test;

TEST_CASE( "a hook written in Luau vetoes through the bus", "[loader]" ) {
	// The exit criterion names a provider, a tool, AND a hook as the three
	// things that must be implementable in the extension language. This is the
	// hook: registered by init.luau, dispatched by the bus, and its veto surfaced
	// with attribution.
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

	// A tool call the hook does not object to passes.
	auto allowed = events::event{ };
	allowed.type = events::kind::tool_pre_call;
	allowed.sequence = 1;
	allowed.payload_json = R"({"name":"read"})";

	REQUIRE_FALSE( static_cast< bool >( bus.publish( allowed ) ) );

	// And the one it does object to is blocked, with the extension named as the
	// source -- the reason is surfaced to the model attributed
	// to the vetoing extension.
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

	// A non-vetoable kind never vetoes, even from the same extension: the handler
	// returns nothing for it, and a veto is only accepted on the three
	// pre-action kinds.
	auto notification = events::event{ };
	notification.type = events::kind::tool_result;
	notification.sequence = 3;
	notification.payload_json = R"({"name":"forbidden"})";

	REQUIRE_FALSE( static_cast< bool >( bus.publish( notification ) ) );
}

TEST_CASE( "a custom event reaches the extension and never the log", "[loader]" ) {
	// The closed tagged union exists so the session log's schema stays
	// explicit. An extension-invented event name must not become a kind, so custom
	// events dispatch inside the hook registry and never touch the bus.
	//
	// This test also guards a lifetime rule that bit once: everything the API
	// surface or a hook holds must outlive the LOAD, not just the loop iteration
	// that created it. The handler here logs through `mcode.log.*`, which reads the
	// extension's manifest -- so if the surface kept a pointer to the loader's
	// local manifest, this is the call that reads freed memory.
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

	// Firing it runs the handler. The handler logs, so the observable effect is
	// that dispatch completes without a failure being counted.
	REQUIRE( static_cast< bool >( hooks.emit( "hello-tool.ready", R"({"count":7})" ) ) );
	REQUIRE( hooks.total_failures( ) == 0 );
	REQUIRE( hooks.failures( "hello-tool" ) == 0 );

	// An unsubscribed name is a no-op rather than an error.
	REQUIRE( static_cast< bool >( hooks.emit( "hello-tool.nothing", "{}" ) ) );

	// And a session event name cannot be emitted: it belongs to the bus, and
	// letting an extension synthesize one would corrupt the log's ordering.
	auto refused = hooks.emit( "tool.call", "{}" );
	REQUIRE_FALSE( static_cast< bool >( refused ) );
	REQUIRE( refused.error( ).msg.find( "session event" ) != std::string::npos );
}

TEST_CASE( "a throwing hook is contained, counted, and does not abort dispatch", "[loader]" ) {
	// Dispatch is noexcept at the bus boundary. A throwing subscriber is
	// caught and counted, and a peer's handler still runs -- a peer's error must
	// never skip another extension's hook.
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

	// The first handler throws; the second still runs and still vetoes.
	auto veto = bus.publish( event );

	REQUIRE( static_cast< bool >( veto ) );

	if ( veto ) {
		REQUIRE( veto->reason == "second handler ran" );
	}

	// The failure was counted rather than thrown, which is what feeds the
	// quarantine threshold.
	REQUIRE( hooks.total_failures( ) >= 1 );
	REQUIRE( hooks.failures( "throwing-hook" ) >= 1 );

	std::filesystem::remove_all( root );
}

TEST_CASE( "a clean call clears the consecutive-failure counter", "[loader]" ) {
	// CONSECUTIVE failures are counted, so an extension that fails once every
	// hundred events is never quarantined. A cumulative counter would quarantine
	// it eventually, which is the wrong behaviour.
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
	// `tool.precal` is a typo, and a hook that can never fire is worse than a load
	// error: the author would believe their guard was active.
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
	// A hook left subscribed to a discarded VM is a crash waiting for the next
	// event, so detaching is part of unload rather than a separate concern.
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

	// Publishing after detach finds no handler and does not reach the dead VM.
	auto event = events::event{ };
	event.type = events::kind::tool_pre_call;
	event.payload_json = R"({"name":"forbidden"})";

	REQUIRE_FALSE( static_cast< bool >( bus.publish( event ) ) );
}

TEST_CASE( "a failing extension leaves nothing behind in the shared registries", "[loader]" ) {
	// An extension can register a hook and a tool and THEN fail -- a typo further
	// down init.luau, an unsupported call, anything. The loader discards its VM,
	// so anything it registered must be discarded with it. A hook that outlives its
	// VM is dispatched by the bus into freed memory; a tool that outlives its VM is
	// visible to the model and impossible to call.
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

	// The tool must be gone from the registry the model reads.
	CHECK( registry.find( "ghost" ) == nullptr );
	CHECK( registry.owned_by( "partial-ext" ).empty( ) );

	// And the hook must be gone from the registry the bus dispatches through.
	// Without this, the next tool.pre_call reaches a destroyed VM.
	CHECK( hooks.handlers_for( "tool.pre_call" ) == 0 );
	CHECK( hooks.size( ) == 0 );

	// Dispatching must be safe: no handler, so no call into the dead VM.
	auto event = events::event{ };
	event.type = events::kind::tool_pre_call;
	event.payload_json = R"({"name":"anything"})";

	CHECK_FALSE( static_cast< bool >( bus.publish( event ) ) );

	std::filesystem::remove_all( root );
}

TEST_CASE( "require loads a module from inside the extension", "[loader]" ) {
	// `require` is a documented part of the surface, and the host option that
	// backs it is the only place path confinement is enforced. Without a test,
	// a regression that stops calling the loader looks like "module not found".
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

	// The tool returns the module's value, which proves the module body ran and
	// its return value was cached and returned to the caller.
	auto called = loaded.invoke( "greet", "{}" );
	REQUIRE( static_cast< bool >( called ) );

	if ( called ) {
		REQUIRE( *called == "hello from a module" );
	}

	std::filesystem::remove_all( root );
}

TEST_CASE( "require cannot escape the extension root", "[loader]" ) {
	// The loader is the only thing standing between an extension and the whole
	// filesystem, and a cloned repository is untrusted input.
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

	// The require is refused, so the extension fails to load rather than reading
	// a file outside its directory.
	REQUIRE( loaded.report.loaded.empty( ) );
	REQUIRE( loaded.report.failed.size( ) == 1 );
	REQUIRE( loaded.report.failed.front( ).reason.find( "module not found" ) != std::string::npos );

	std::filesystem::remove_all( root );
}

TEST_CASE( "five consecutive handler failures quarantine the extension", "[loader]" ) {
	// Counting the failures without enforcing the threshold is a counter, not a
	// quarantine. This is the test that the counter is wired to the consequence:
	// every handler detached, and the extension refused if it tries again.
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

	// Four failures keep the subscription: the threshold is five CONSECUTIVE.
	for ( auto attempt = 0; attempt < 4; ++attempt ) {
		bus.publish( event );
	}

	REQUIRE( hooks.failures( "always-throws" ) == 4 );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 1 );
	REQUIRE_FALSE( hooks.quarantined( "always-throws" ) );

	// The fifth trips it: every handler for the extension is detached.
	bus.publish( event );

	REQUIRE( hooks.quarantined( "always-throws" ) );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 0 );
	REQUIRE( hooks.size( ) == 0 );

	// And it cannot quietly re-register its way back.
	auto refused = hooks.subscribe( *loaded.extensions.front( ).host, "tool.pre_call", 0,
		"always-throws" );

	REQUIRE_FALSE( static_cast< bool >( refused ) );
	REQUIRE( refused.error( ).msg.find( "quarantined" ) != std::string::npos );

	std::filesystem::remove_all( root );
}

TEST_CASE( "a clean call clears the consecutive-failure streak", "[loader]" ) {
	// The counter is CONSECUTIVE, so an extension that fails intermittently is
	// never quarantined. A cumulative counter would eventually remove a handler
	// that works most of the time, which is the wrong trade.
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

	// Four failures, then a clean call, repeated: the streak never reaches five.
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
