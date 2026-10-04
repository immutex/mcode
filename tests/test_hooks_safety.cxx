#include "ext_test_helpers.hxx"

#include <filesystem>
#include <string>

using namespace mcode;
using namespace ext_test;


TEST_CASE( "a handler that unsubscribes itself mid-dispatch keeps its veto", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "self-off";

	write( directory / "ext.toml",
		"name = \"self-off\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(local mine

mine = mcode.on("tool.pre_call", function(ev)
	-- the subscription this callback is running under, dropped from inside the call
	mcode.off(mine)

	if ev.payload.name == "forbidden" then
		return { veto = "vetoed after unsubscribing itself" }
	end

	return nil
end)

-- subscribed second, so erasing the vetoable entry above shifts this non-vetoable one
-- into its slot: reading the erased element's fields would lose the veto below.
mcode.on("tool.result", function(ev)
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

	INFO( describe( loaded.report ) );
	REQUIRE( loaded.report.loaded.size( ) == 1 );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 1 );

	auto blocked = events::event{ };
	blocked.type = events::kind::tool_pre_call;
	blocked.payload_json = R"({"name":"forbidden"})";

	auto veto = bus.publish( blocked );

	// the veto is built after the erase, so it proves the fields were read before the pcall
	REQUIRE( static_cast< bool >( veto ) );

	if ( veto ) {
		REQUIRE( veto->reason == "vetoed after unsubscribing itself" );
		REQUIRE( veto->source == "self-off" );
	}

	REQUIRE( hooks.size( ) == 1 );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 0 );
	REQUIRE( hooks.total_failures( ) == 0 );

	auto again = events::event{ };
	again.type = events::kind::tool_pre_call;
	again.payload_json = R"({"name":"forbidden"})";

	REQUIRE_FALSE( static_cast< bool >( bus.publish( again ) ) );

	std::filesystem::remove_all( root );
}

TEST_CASE( "an extension cannot unsubscribe another extension's hook", "[loader]" ) {
	const auto root = scratch_root( );

	// a-guard loads first, so its subscription is id 1 and the intruder must guess it.
	write( root / "a-guard" / "ext.toml",
		"name = \"a-guard\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( root / "a-guard" / "init.luau", R"LUASRC(mcode.on("tool.pre_call", function(ev)
	if ev.payload.name == "forbidden" then
		return { veto = "blocked by a-guard" }
	end

	return nil
end)
)LUASRC" );

	write( root / "b-intruder" / "ext.toml",
		"name = \"b-intruder\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( root / "b-intruder" / "init.luau", R"LUASRC(-- id 1 is the guard's: refused, so the guard keeps its veto
mcode.off(1)

local mine = mcode.on("tool.pre_call", function(ev)
	return nil
end)

-- its own id still works
mcode.off(mine)
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
	REQUIRE( loaded.report.loaded.size( ) == 2 );
	REQUIRE( loaded.report.failed.empty( ) );

	// the intruder dropped only its own subscription
	REQUIRE( hooks.size( ) == 1 );
	REQUIRE( hooks.handlers_for( "tool.pre_call" ) == 1 );

	auto blocked = events::event{ };
	blocked.type = events::kind::tool_pre_call;
	blocked.payload_json = R"({"name":"forbidden"})";

	auto veto = bus.publish( blocked );

	REQUIRE( static_cast< bool >( veto ) );

	if ( veto ) {
		REQUIRE( veto->source == "a-guard" );
	}

	REQUIRE( hooks.total_failures( ) == 0 );

	std::filesystem::remove_all( root );
}

TEST_CASE( "a timer fires only when the host pumps, and stop cancels it", "[loader]" ) {
	const auto root = scratch_root( );
	const auto directory = root / "timers";

	write( directory / "ext.toml",
		"name = \"timers\"\nversion = \"0.1.0\"\napi_version = 1\n" );
	write( directory / "init.luau", R"LUASRC(local fired = 0
local cancelled = 0

mcode.timer.at(0, function()
	fired = fired + 1
end)

local handle = mcode.timer.at(0, function()
	cancelled = cancelled + 1
end)

handle:stop()

mcode.tool.register({
	name = "counters",
	description = "Reports how many timers fired",
	permission = "read",
	schema = { type = "object", properties = {} },
	run = function(_args, _context)
		return tostring(fired) .. ":" .. tostring(cancelled), nil
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

	const auto before = loaded.invoke( "counters", "{}" );

	REQUIRE( static_cast< bool >( before ) );

	if ( before ) {
		REQUIRE( *before == "0:0" );
	}

	loaded.pump_timers( );

	const auto after = loaded.invoke( "counters", "{}" );

	REQUIRE( static_cast< bool >( after ) );

	if ( after ) {
		// a single-shot timer fires once; the stopped one never fires at all
		REQUIRE( *after == "1:0" );
	}

	loaded.pump_timers( );

	const auto again = loaded.invoke( "counters", "{}" );

	REQUIRE( static_cast< bool >( again ) );

	if ( again ) {
		REQUIRE( *again == "1:0" );
	}

	std::filesystem::remove_all( root );
}
