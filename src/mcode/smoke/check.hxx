#pragma once

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

auto smoke_cli_and_extensions( ) -> void;

auto smoke_luau( ) -> void;
