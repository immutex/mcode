#pragma once

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "mcode/events/bus.hxx"
#include "mcode/ext/api_cmd.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/model/provider.hxx"

#include "test_scratch.hxx"

namespace ext_test {

	// a free function cannot capture, so the installer reads the registry from here.
	inline mcode::tool_registry* g_registry = nullptr;

	// Notifications an extension raised, newest last, as "level: message". Without
	// an installed sink `mcode.notify` is a silent no-op, so an assertion on one
	// would pass against nothing.
	inline std::vector< std::string > g_notifications;

	inline auto g_notifier = std::function< void( const std::string&, const std::string&,
		const std::string& ) >{ []( const std::string& name, const std::string& level,
		const std::string& message ) {
		g_notifications.push_back( name + " " + level + ": " + message );
	} };

	// Commands the loaded extensions registered. A surface always has one in
	// production, so the helper always does too: a null here would make
	// `mcode.cmd.register` raise and mask the wiring this is meant to exercise.
	inline mcode::ext::command_registry g_commands;

	// Queued `mcode.defer` callbacks, drained by `pump_deferred`.
	inline std::vector< int > g_deferred;

	inline auto g_session_state = std::function< mcode::ext::session_state( ) >{ []( ) {
		auto state = mcode::ext::session_state{ };
		state.session = "fixture-session";
		state.seq = 7;
		state.steps = 3;

		return state;
	} };

	inline std::vector< std::string > g_instructions;

	inline auto g_instruction_sink = std::function< void( const std::string&,
		const std::string& ) >{ []( const std::string& owner, const std::string& text ) {
		g_instructions.push_back( owner + ": " + text );
	} };

	inline auto extensions_root( ) -> std::filesystem::path {
		return std::filesystem::path{ MCODE_FIXTURE_EXTENSIONS };
	}

	// The real API surface, not a stub, so an API rename cannot silently drift. It
	// must install the same collaborators production wires: a test that installs a
	// partial surface passes against one, which is how `cmd.register`, `defer` and
	// `session.snapshot` were verified while inert.
	inline auto register_api( const mcode::ext::registration& given )
		-> mcode::status {
		const auto context = mcode::ext::register_context{ .commands = &g_commands,
			.session_state = &g_session_state, .notifier = &g_notifier,
			.instruction_sink = &g_instruction_sink };

		return mcode::ext::default_register_api( *g_registry, context )( given );
	}

	// The fixtures that prove a gated API entry works live apart from the general
	// fixtures, so a test that counts the general set does not have to know about them.
	inline auto wiring_root( ) -> std::filesystem::path {
		return std::filesystem::path{ MCODE_WIRING_FIXTURES };
	}

	// a bare "0 == 1" hides which extension failed and why.
	inline auto describe( const mcode::ext::load_report& report ) -> std::string {
		auto text = std::string{ };

		for ( const auto& failure : report.failed ) {
			text += "\n  failed " + failure.name + ": " + failure.reason;
		}

		for ( const auto& entry : report.loaded ) {
			text += "\n  loaded " + entry.name + " (" +
				std::to_string( entry.tools.size( ) ) + " tools)";
		}

		return text;
	}

	inline auto scratch_root( ) -> std::filesystem::path {
		return mcode::test::scratch_directory( "mcode-loader-test" );
	}

	inline auto write( const std::filesystem::path& path, const std::string_view text ) -> void {
		std::filesystem::create_directories( path.parent_path( ) );

		auto out = std::ofstream{ path, std::ios::trunc };
		out << text;
	}

}
