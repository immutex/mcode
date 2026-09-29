#pragma once

// Every scratch path in the suite must come from here: agents run `ctest` in
// parallel worktrees, and a fixed path (or an unseeded ::rand) makes two runs
// delete each other's fixtures, which surfaces as failures in unrelated tests.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mcode::test {

	// A directory unique to this process and this call. The path cannot pre-exist,
	// so the helper creates it and never removes an existing directory.
	inline auto scratch_directory( const std::string_view prefix ) -> std::filesystem::path {
		static auto calls = std::atomic< std::uint64_t >{ 0 };

		const auto stamp = std::chrono::steady_clock::now( ).time_since_epoch( ).count( );
		const auto name = std::string{ prefix } + "-" + std::to_string( stamp ) + "-"
			+ std::to_string( ::GetCurrentProcessId( ) ) + "-"
			+ std::to_string( calls.fetch_add( 1, std::memory_order_relaxed ) );

		auto path = std::filesystem::temp_directory_path( ) / name;
		std::filesystem::create_directories( path );

		return path;
	}

}

namespace test = mcode::test;
