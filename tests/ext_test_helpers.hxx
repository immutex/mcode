#pragma once

// Shared by test_manifest.cxx, test_loader.cxx and test_hooks.cxx. The three
// were one file past the 600-line limit; these helpers are what they share.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "mcode/events/bus.hxx"
#include "mcode/ext/hooks.hxx"
#include "mcode/ext/loader.hxx"
#include "mcode/model/provider.hxx"

#include "test_scratch.hxx"

namespace ext_test {

	// The registry a load writes into. A free function cannot capture, so the
	// installer reads it from here; each test replaces it before loading.
	inline mcode::tool_registry* g_registry = nullptr;

	inline auto extensions_root( ) -> std::filesystem::path {
		return std::filesystem::path{ MCODE_FIXTURE_EXTENSIONS };
	}

	// The REAL API surface, not a stub. A hand-written stub would keep passing
	// after the API renamed something, which is exactly the drift this suite
	// exists to catch -- the extension calls `mcode.tool.register` through the
	// same entry points a third-party extension does.
	inline auto register_api( const mcode::ext::registration& given )
		-> mcode::status {
		return given.surface.install( mcode::ext::api_surface::install_request{ .host = given.host,
			.registry = *g_registry, .providers = given.providers, .hooks = given.hooks,
			.details = given.details } );
	}

	// A bare "0 == 1" hides which extension failed and why, so every assertion on
	// the report goes through this.
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
