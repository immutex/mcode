#pragma once

// The startup smoke test's shared state and reporting helpers.
//
// Split out so the smoke checks can live in more than one translation unit:
// main.cxx is the driver, and a single file listing every check grew past the
// project's file-length limit. The counters are inline variables rather than
// extern definitions, so a check in any TU increments the same totals.

#include <cstdio>
#include <string_view>

namespace smoke {

	inline int g_failures = 0;
	inline int g_checks = 0;

	inline auto check( const bool condition, const std::string_view label,
		const std::string_view detail = { } ) -> void {
		++g_checks;

		if ( condition ) {
			std::printf( "  [ ok ] %.*s\n", static_cast< int >( label.size( ) ), label.data( ) );

			return;
		}

		++g_failures;

		std::printf( "  [FAIL] %.*s", static_cast< int >( label.size( ) ), label.data( ) );

		if ( !detail.empty( ) ) {
			std::printf( " -- %.*s", static_cast< int >( detail.size( ) ), detail.data( ) );
		}

		std::printf( "\n" );
	}

	inline auto section( const std::string_view title ) -> void {
		std::printf( "\n== %.*s ==\n", static_cast< int >( title.size( ) ), title.data( ) );
	}

}

// Defined in smoke_cli_extensions.cxx. Split so no single smoke file exceeds the
// project's file-length limit.
auto smoke_cli_and_extensions( ) -> void;
