#pragma once

// every scratch path must be unique per process: parallel ctest runs delete each other's fixtures.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mcode::test {

	[[nodiscard]] inline auto process_id( ) -> unsigned long long {
	#if defined( _WIN32 )
		return static_cast< unsigned long long >( ::GetCurrentProcessId( ) );
	#else
		return static_cast< unsigned long long >( ::getpid( ) );
	#endif
	}

	// unique per process and per call, so the path can never pre-exist.
	inline auto scratch_directory( const std::string_view prefix ) -> std::filesystem::path {
		static auto calls = std::atomic< std::uint64_t >{ 0 };

		const auto stamp = std::chrono::steady_clock::now( ).time_since_epoch( ).count( );
		const auto name = std::string{ prefix } + "-" + std::to_string( stamp ) + "-"
			+ std::to_string( process_id( ) ) + "-"
			+ std::to_string( calls.fetch_add( 1, std::memory_order_relaxed ) );

		auto path = std::filesystem::temp_directory_path( ) / name;
		std::filesystem::create_directories( path );

		return path;
	}

}

namespace test = mcode::test;
