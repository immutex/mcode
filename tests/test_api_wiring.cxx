#include "ext_test_helpers.hxx"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

#include "mcode/cli/slash.hxx"
#include "mcode/ext/api_cmd.hxx"

using namespace mcode;
using namespace ext_test;

namespace {

	// Loads only the wiring fixture, so an unrelated fixture cannot satisfy the
	// assertion by accident.
	auto load_wired( ext::command_registry& commands, ext::load_result& out ) -> void {
		auto registry = tool_registry{ };
		g_registry = &registry;
		g_commands = ext::command_registry{ };
		g_notifications.clear( );
		g_instructions.clear( );

		auto providers = model::provider_registry{ };
		auto bus = events::bus{ };
		auto hooks = ext::hook_registry{ bus };

		auto options = ext::loader_options{ };
		options.register_api = register_api;

		out = ext::load_extensions( { wiring_root( ) }, providers, hooks, options );

		commands = g_commands;
	}

}

TEST_CASE( "an extension command is registered and runnable", "[api][wiring]" ) {
	auto commands = ext::command_registry{ };
	auto loaded = ext::load_result{ };

	load_wired( commands, loaded );

	INFO( describe( loaded.report ) );
	REQUIRE( loaded.report.failed.empty( ) );

	// Registered: without the command collaborator `mcode.cmd.register` raises and
	// the load fails, so reaching here at all is half the assertion.
	REQUIRE( commands.find( "wired-cmd" ) != nullptr );

	// Runnable: this is the path that had no caller, so a command was unreachable
	// even once registered.
	const auto ran = loaded.invoke_command( commands, "wired-cmd", "hello" );
	REQUIRE( ran.has_value( ) );
	REQUIRE( ran->find( "ran with: hello" ) != std::string::npos );
}

TEST_CASE( "an extension command reaches the palette", "[api][wiring]" ) {
	auto commands = ext::command_registry{ };
	auto loaded = ext::load_result{ };

	load_wired( commands, loaded );
	REQUIRE( loaded.report.failed.empty( ) );

	const auto rows = cli::all_commands( commands );

	const auto found = std::ranges::find_if( rows, []( const tui::slash_command& row ) {
		return row.name == "wired-cmd";
	} );

	REQUIRE( found != rows.end( ) );
	REQUIRE( found->description == "A command an extension registered" );

	// A builtin still wins its own name, so an extension cannot shadow `/exit`.
	const auto exits = std::ranges::count_if( rows, []( const tui::slash_command& row ) {
		return row.name == "exit";
	} );

	REQUIRE( exits == 1 );
}

TEST_CASE( "a deferred callback runs on the pump, not inside the call", "[api][wiring]" ) {
	auto commands = ext::command_registry{ };
	auto loaded = ext::load_result{ };

	load_wired( commands, loaded );
	REQUIRE( loaded.report.failed.empty( ) );

	// The fixture defers a notification. Until the pump runs, it must not have fired.
	const auto before = std::ranges::count_if( g_notifications,
		[]( const std::string& note ) { return note.find( "deferred ran" ) != std::string::npos; } );

	REQUIRE( before == 0 );

	loaded.pump_deferred( );

	const auto after = std::ranges::count_if( g_notifications,
		[]( const std::string& note ) { return note.find( "deferred ran" ) != std::string::npos; } );

	REQUIRE( after == 1 );

	// Drained, not re-run: a second pump has nothing left.
	loaded.pump_deferred( );

	const auto again = std::ranges::count_if( g_notifications,
		[]( const std::string& note ) { return note.find( "deferred ran" ) != std::string::npos; } );

	REQUIRE( again == 1 );
}

TEST_CASE( "session.snapshot reads the installed session state", "[api][wiring]" ) {
	auto commands = ext::command_registry{ };
	auto loaded = ext::load_result{ };

	load_wired( commands, loaded );
	REQUIRE( loaded.report.failed.empty( ) );

	// The fixture reports the snapshot through notify, so the values prove the
	// callback was installed rather than returning the "no session context" nil.
	const auto reported = std::ranges::find_if( g_notifications, []( const std::string& note ) {
		return note.find( "session fixture-session seq 7" ) != std::string::npos;
	} );

	REQUIRE( reported != g_notifications.end( ) );
}

TEST_CASE( "context.add_instructions reaches the instruction sink", "[api][wiring]" ) {
	auto commands = ext::command_registry{ };
	auto loaded = ext::load_result{ };

	load_wired( commands, loaded );
	REQUIRE( loaded.report.failed.empty( ) );

	const auto contributed = std::ranges::find_if( g_instructions, []( const std::string& text ) {
		return text == "wired: the wired fixture contributed this";
	} );

	REQUIRE( contributed != g_instructions.end( ) );
}
